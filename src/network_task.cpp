#include "network_task.h"
#include "config.h"
#include "state_bus.h"
#include "ota_update.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mqtt_client.h"

// Real QoS 1 (broker acks + automatic resend) requires ESP-IDF's
// esp_mqtt_client - the common Arduino MQTT library (PubSubClient) only
// ever does QoS 0 on publish regardless of what flag you pass it. This file
// is the only place in the firmware that talks MQTT; it runs entirely
// inside the Core-0-pinned network task (see main.cpp).

static esp_mqtt_client_handle_t s_client = nullptr;
static char s_mqttUri[160];

// ---- Retained-state resync cache ----
// Core 1 keeps running/timing its own devices for the entire time the
// network is down (per spec) and does NOT re-send anything when the link
// comes back - resync on reconnect is this task's job alone. Each retained
// topic's last-known JSON is cached here so MQTT_EVENT_CONNECTED can
// republish everything immediately, without waiting for Core 1 to notice a
// state change again (there may not be one - the device could be sitting
// perfectly idle through the whole outage).
struct TopicCache { bool valid; char json[128]; };
static TopicCache s_ledCache{false, ""};
static TopicCache s_pumpCache{false, ""};
static TopicCache s_mistCache{false, ""};
static TopicCache s_solenoidCache{false, ""};
static TopicCache s_motorCache{false, ""};

static const char *topicForState(StateTopicId id) {
  switch (id) {
    case STATE_LED:            return TOPIC_LED_STATE;
    case STATE_PUMP:           return TOPIC_PUMP_STATE;
    case STATE_MIST:           return TOPIC_MIST_STATE;
    case STATE_SOLENOID:       return TOPIC_SOLENOID_STATE;
    case STATE_SOLENOID_ERROR: return TOPIC_SOLENOID_ERROR;
    case STATE_MOTOR:          return TOPIC_MOTOR_STATE;
    case STATE_MQ135_READING:  return TOPIC_MQ135_READING;
  }
  return nullptr;
}

// Retained: the topics a fresh dashboard load needs an instant value for.
// Not retained: one-shot events (sensor readings, conflict reports) that
// shouldn't linger forever as a stale "last reading"/"last error".
static bool isRetainedTopic(StateTopicId id) {
  switch (id) {
    case STATE_LED: case STATE_PUMP: case STATE_MIST:
    case STATE_SOLENOID: case STATE_MOTOR:
      return true;
    default:
      return false;
  }
}

static TopicCache *cacheForState(StateTopicId id) {
  switch (id) {
    case STATE_LED:      return &s_ledCache;
    case STATE_PUMP:     return &s_pumpCache;
    case STATE_MIST:     return &s_mistCache;
    case STATE_SOLENOID: return &s_solenoidCache;
    case STATE_MOTOR:    return &s_motorCache;
    default: return nullptr;
  }
}

static void publishOne(StateTopicId id, const char *json) {
  if (!g_mqttConnected) return;   // dropped, not queued - the retained cache + resyncAllRetained() is the recovery path, not a resend buffer here
  const char *topic = topicForState(id);
  if (!topic) return;
  esp_mqtt_client_publish(s_client, topic, json, 0, 1 /* QoS1 */, isRetainedTopic(id) ? 1 : 0);
}

void network_publish_ota_status(const char *status) {
  if (!g_mqttConnected || !s_client) return;
  esp_mqtt_client_publish(s_client, TOPIC_OTA_STATUS, status, 0, 1, 1);
}

static void resyncAllRetained() {
  if (s_ledCache.valid)      publishOne(STATE_LED, s_ledCache.json);
  if (s_pumpCache.valid)     publishOne(STATE_PUMP, s_pumpCache.json);
  if (s_mistCache.valid)     publishOne(STATE_MIST, s_mistCache.json);
  if (s_solenoidCache.valid) publishOne(STATE_SOLENOID, s_solenoidCache.json);
  if (s_motorCache.valid)    publishOne(STATE_MOTOR, s_motorCache.json);
}

// Parses one incoming command message and forwards it to Core 1 as a
// Command struct. This function ONLY parses - it never decides thresholds,
// never touches a pin, and never blocks; xQueueSend below uses a 0 tick
// timeout so a momentarily-full queue just drops the command instead of
// stalling this task (which would also stall MQTT's own keepalive/ack
// processing).
static void handleIncomingCommand(const char *topic, const char *payload, int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

  Command cmd{};
  bool have = false;

  if (strcmp(topic, TOPIC_LED_SET) == 0 && doc["on"].is<bool>()) {
    cmd.type = CMD_LED_SET; cmd.boolValue = doc["on"]; have = true;
  } else if (strcmp(topic, TOPIC_PUMP_SET) == 0 && doc["on"].is<bool>()) {
    cmd.type = CMD_PUMP_SET; cmd.boolValue = doc["on"]; have = true;
  } else if (strcmp(topic, TOPIC_MIST_SET) == 0) {
    if (doc["pulse_s"].is<int>()) {
      cmd.type = CMD_MIST_PULSE; cmd.intValue = doc["pulse_s"]; have = true;
    } else if (doc["auto"].is<bool>()) {
      cmd.type = CMD_MIST_AUTO_SET; cmd.boolValue = doc["auto"]; have = true;
    }
  } else if (strcmp(topic, TOPIC_SOLENOID_SET) == 0) {
    if (doc["pulse"].is<bool>() && (bool)doc["pulse"]) {
      cmd.type = CMD_SOLENOID_PULSE; have = true;
    } else if (doc["hold"].is<bool>()) {
      cmd.type = CMD_SOLENOID_HOLD_SET; cmd.boolValue = doc["hold"]; have = true;
    }
  } else if (strcmp(topic, TOPIC_MOTOR_SET) == 0 && doc["speed"].is<int>()) {
    cmd.type = CMD_MOTOR_SET; cmd.intValue = doc["speed"]; have = true;
  } else if (strcmp(topic, TOPIC_MOTOR_EXTERNAL_TRIGGER) == 0 && doc["request"].is<bool>()) {
    cmd.type = CMD_EXTERNAL_TRIGGER_SET; cmd.boolValue = doc["request"]; have = true;
  } else if (strcmp(topic, TOPIC_EXTERNAL_PM_READING) == 0 && doc["co2"].is<float>() && doc["pm25"].is<float>()) {
    cmd.type = CMD_EXTERNAL_READING_SET;
    cmd.externalReading.co2 = doc["co2"];
    cmd.externalReading.pm25 = doc["pm25"];
    have = true;
  }

  if (have) xQueueSend(cmdQueueHandle, &cmd, 0);
}

// OTA is Core-0-only (network_task's own domain, same as MQTT itself) - a
// trigger message just sets a flag for ota_poll() to act on next tick,
// rather than kicking off the (blocking, multi-second) download from inside
// this MQTT event callback.
static void handleIncomingTopic(const char *topic, const char *payload, int len) {
  if (strcmp(topic, TOPIC_OTA_TRIGGER) == 0) {
    ota_request_check();
    return;
  }
  handleIncomingCommand(topic, payload, len);
}

static esp_err_t mqttEventHandler(esp_mqtt_event_handle_t event) {
  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      g_mqttConnected = true;
      esp_mqtt_client_subscribe(s_client, TOPIC_LED_SET, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_PUMP_SET, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_MIST_SET, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_SOLENOID_SET, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_MOTOR_SET, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_MOTOR_EXTERNAL_TRIGGER, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_EXTERNAL_PM_READING, 1);
      esp_mqtt_client_subscribe(s_client, TOPIC_OTA_TRIGGER, 1);
      esp_mqtt_client_publish(s_client, TOPIC_STATUS, "online", 0, 1, 1);
      resyncAllRetained();
      break;
    case MQTT_EVENT_DISCONNECTED:
      g_mqttConnected = false;
      break;
    case MQTT_EVENT_DATA: {
      char topicBuf[128];
      int tlen = event->topic_len < (int)sizeof(topicBuf) - 1 ? event->topic_len : (int)sizeof(topicBuf) - 1;
      memcpy(topicBuf, event->topic, tlen);
      topicBuf[tlen] = '\0';
      handleIncomingTopic(topicBuf, event->data, event->data_len);
      break;
    }
    default:
      break;
  }
  return ESP_OK;
}

// esp_mqtt_client_register_event's handler signature (the esp_event style)
// has been the same across IDF4 and IDF5's mqtt component - only the config
// struct's field layout differs between them (handled below in mqttInit).
static void mqttEventHandlerAdapter(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  mqttEventHandler((esp_mqtt_event_handle_t)event_data);
}

static void mqttInit() {
  snprintf(s_mqttUri, sizeof(s_mqttUri), MQTT_USE_TLS ? "mqtts://%s:%d" : "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);

  esp_mqtt_client_config_t cfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.uri = s_mqttUri;
#if MQTT_USE_TLS
  cfg.broker.verification.certificate = MQTT_ROOT_CA;
#endif
  cfg.credentials.username = MQTT_USERNAME;
  cfg.credentials.authentication.password = MQTT_PASSWORD;
  cfg.session.last_will.topic = TOPIC_STATUS;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 0;   // 0 = use strlen(msg)
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  // We drive reconnection ourselves with exponential backoff (see
  // wifiSupervisor/mqttSupervisor below) instead of IDF's built-in fixed-
  // interval auto-reconnect, per the spec's "exponential backoff on Core 0" requirement.
  cfg.network.disable_auto_reconnect = true;
#else
  // IDF4-style flat config, for older arduino-esp32 core / platform releases.
  cfg.uri = s_mqttUri;
#if MQTT_USE_TLS
  cfg.cert_pem = MQTT_ROOT_CA;
#endif
  cfg.username = MQTT_USERNAME;
  cfg.password = MQTT_PASSWORD;
  cfg.lwt_topic = TOPIC_STATUS;
  cfg.lwt_msg = "offline";
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.disable_auto_reconnect = true;
#endif

  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client) {
    // Confirmed on real hardware: an invalid MQTT_HOST (e.g. the config.h
    // placeholder, which contains underscores the IDF URI parser rejects)
    // makes esp_mqtt_client_init() fail and return NULL here - the library
    // itself logs "Error parse uri" and degrades gracefully (no crash), but
    // s_client staying NULL forever would otherwise be a silent dead end.
    // mqttSupervisor() below checks this same flag before ever touching
    // s_client again, so a bad broker config fails loud once at boot
    // instead of a null-handle call path relying on the library's own
    // (undocumented) tolerance for it.
    Serial.println("[mqtt] esp_mqtt_client_init() failed - check MQTT_HOST/MQTT_PORT in config.h");
    return;
  }
  esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqttEventHandlerAdapter, nullptr);
  esp_mqtt_client_start(s_client);
}

// ---- Exponential backoff supervisors ----
// WiFi and MQTT are tracked independently: either one can be the thing
// that's actually down (broker unreachable with WiFi fine, or vice versa).
// Both reset their backoff to the minimum on a successful (re)connect and
// double up to a cap on repeated failures - this never delays or blocks
// Core 1, it only paces how often THIS task retries.
// BACKOFF_MIN_MS is also used as the grace period after starting/
// reconnecting MQTT before we're willing to force another reconnect - a
// real TLS handshake to an internet broker (HiveMQ Cloud etc) can
// legitimately take several seconds on an ESP32's CPU, and 1000ms was
// confirmed on hardware to be too aggressive: mqttSupervisor() was calling
// esp_mqtt_client_reconnect() roughly once a second, repeatedly aborting
// the in-progress handshake before it could ever finish, with no error
// ever logged (it wasn't failing, it was being restarted). 8s gives a real
// handshake attempt a fair chance to either succeed or genuinely fail
// before we intervene.
#define BACKOFF_MIN_MS 8000UL
#define BACKOFF_MAX_MS 60000UL

static unsigned long s_wifiBackoffMs = BACKOFF_MIN_MS;
static unsigned long s_wifiNextAttemptAt = 0;
static bool s_wifiWasConnected = false;

static void wifiSupervisor(unsigned long now) {
  bool connected = (WiFi.status() == WL_CONNECTED);
  digitalWrite(PIN_WIFI_LED, connected ? HIGH : LOW);
  if (connected) {
    if (!s_wifiWasConnected) s_wifiBackoffMs = BACKOFF_MIN_MS;
    s_wifiWasConnected = true;
    return;
  }
  s_wifiWasConnected = false;
  if (now < s_wifiNextAttemptAt) return;
  s_wifiNextAttemptAt = now + s_wifiBackoffMs;
  s_wifiBackoffMs = min(s_wifiBackoffMs * 2, BACKOFF_MAX_MS);
  WiFi.reconnect();
}

static unsigned long s_mqttBackoffMs = BACKOFF_MIN_MS;
static unsigned long s_mqttNextAttemptAt = 0;

static bool s_mqttInitAttempted = false;

static void mqttSupervisor(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;   // no point starting/retrying MQTT without a network path

  if (!s_mqttInitAttempted) {
    s_mqttInitAttempted = true;
    // Confirmed on real hardware: calling esp_mqtt_client_start() before
    // WiFi has an actual IP/route makes the very first connect attempt
    // fail with "Host is unreachable" - and unlike a normal disconnect,
    // that leaves the client stuck (a later esp_mqtt_client_reconnect()
    // never produces another connect attempt, logged or otherwise).
    // Deferring mqttInit() to here - the first tick where WiFi is already
    // connected - avoids ever hitting that state.
    mqttInit();
    // Also confirmed on hardware: without this, s_mqttNextAttemptAt stays
    // at its default 0, so the very NEXT tick (20ms later) would already
    // satisfy `now < s_mqttNextAttemptAt` as false and fire an immediate
    // esp_mqtt_client_reconnect() - aborting the handshake esp_mqtt_client_start()
    // just kicked off before it ever had a chance to complete. This grace
    // period is what actually gives the first attempt time to finish.
    s_mqttNextAttemptAt = now + BACKOFF_MIN_MS;
    return;   // esp_mqtt_client_start() inside mqttInit() already attempts a connection this tick
  }

  if (!s_client) return;   // esp_mqtt_client_init() failed (e.g. malformed MQTT_HOST) - nothing to reconnect
  if (g_mqttConnected) {
    s_mqttBackoffMs = BACKOFF_MIN_MS;
    return;
  }
  if (now < s_mqttNextAttemptAt) return;
  s_mqttNextAttemptAt = now + s_mqttBackoffMs;
  s_mqttBackoffMs = min(s_mqttBackoffMs * 2, BACKOFF_MAX_MS);
  esp_mqtt_client_reconnect(s_client);
}

void networkTaskFn(void *pv) {
  // The WiFi status LED isn't one of the five spec'd devices (no relay/PWM
  // single-writer rule applies to it) - this task owns it exclusively since
  // it only ever reflects this task's own connectivity state.
  pinMode(PIN_WIFI_LED, OUTPUT);
  digitalWrite(PIN_WIFI_LED, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // this task owns reconnect timing (exponential backoff) instead of WiFi's built-in immediate retry
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ota_init();

  // mqttInit() is NOT called here - see mqttSupervisor()'s s_mqttInitAttempted
  // gate below, which defers it until WiFi actually has a connection.

  for (;;) {
    unsigned long now = millis();
    wifiSupervisor(now);
    mqttSupervisor(now);
    // Blocks this Core-0 task for the duration of a download+flash-write
    // when an update is actually in progress (typically single-digit
    // seconds over WiFi for a ~1MB image) - that's fine here since Core 1's
    // relay/device logic doesn't depend on this task at all. MQTT keepalive
    // does pause for that same window, which is an accepted, brief trade-off
    // for how rarely this actually fires (see OTA_CHECK_INTERVAL_MS).
    ota_poll(now, g_mqttConnected);

    StateUpdate upd;
    while (xQueueReceive(stateQueueHandle, &upd, 0) == pdTRUE) {
      TopicCache *cache = cacheForState(upd.topic);
      if (cache) {
        cache->valid = true;
        strncpy(cache->json, upd.json, sizeof(cache->json) - 1);
        cache->json[sizeof(cache->json) - 1] = '\0';
      }
      publishOne(upd.topic, upd.json);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

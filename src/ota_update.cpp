#include "ota_update.h"
#include "config.h"

#if OTA_ENABLED

#include "network_task.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <string.h>

// GitHub-pull OTA: fetches a small manifest.json ({"version","url"}) over
// HTTPS, and if its version differs from FIRMWARE_VERSION (config.h),
// downloads and flashes the .bin the manifest points at via esp_https_ota().
// Entirely Core 0 (network_task.cpp's domain) - see the comment on
// ota_poll()'s caller in network_task.cpp for why blocking here is fine.
//
// Uses esp_crt_bundle_attach (Mozilla's trusted-CA bundle, enabled via
// CONFIG_MBEDTLS_CERTIFICATE_BUNDLE in sdkconfig.defaults) rather than a
// hand-embedded certificate, because unlike the MQTT broker's cert (pinned
// once, ours to control), GitHub's cert is a third party's and can rotate.

static bool s_checkRequested = false;
static unsigned long s_nextCheckAt = 0;

void ota_request_check() { s_checkRequested = true; }

void ota_init() {
  Serial.printf("[ota] running firmware version %s\n", FIRMWARE_VERSION);
  // Automatic checks disabled 2026-07-28: confirmed on hardware (twice,
  // via serial monitor) that this boot-time check reliably kills the MQTT
  // connection ~30s after boot - the blocking HTTPS/TLS request to GitHub
  // collides with the MQTT client's own TLS session (esp_crt_bundle "PK
  // verify failed", then MQTT gets "Connection reset by peer" and every
  // reconnect after that fails DNS resolution permanently, until reboot).
  // OTA isn't wired up end-to-end yet anyway (GitHub Actions secrets still
  // not added - see esp32-mqtt-control's repo notes), so there's no
  // upside to auto-checking right now, only a guaranteed outage every
  // boot. Manual checks (publish anything to TOPIC_OTA_TRIGGER) still
  // work below - only the automatic boot/interval trigger is disabled.
  // Restore `millis() + 30000UL` here once the HTTPS/MQTT TLS collision
  // is actually root-caused and fixed.
  s_nextCheckAt = (unsigned long)-1;
}

static bool fetchManifest(char *outVersion, size_t verLen, char *outUrl, size_t urlLen) {
  esp_http_client_config_t cfg = {};
  cfg.url = OTA_MANIFEST_URL;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 8000;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return false;

  bool ok = false;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    esp_http_client_fetch_headers(client);   // return value unused - the read loop below works whether or not Content-Length was present (chunked responses included)
    char buf[512];
    int total = 0;
    int r;
    while (total < (int)sizeof(buf) - 1 &&
           (r = esp_http_client_read(client, buf + total, sizeof(buf) - 1 - total)) > 0) {
      total += r;
    }
    buf[total] = '\0';

    if (total > 0) {
      JsonDocument doc;
      if (deserializeJson(doc, buf, total) == DeserializationError::Ok &&
          doc["version"].is<const char *>() && doc["url"].is<const char *>()) {
        strncpy(outVersion, doc["version"].as<const char *>(), verLen - 1);
        outVersion[verLen - 1] = '\0';
        strncpy(outUrl, doc["url"].as<const char *>(), urlLen - 1);
        outUrl[urlLen - 1] = '\0';
        ok = true;
      }
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

static void performUpdate(const char *url) {
  network_publish_ota_status("updating");
  Serial.printf("[ota] downloading %s\n", url);

  esp_http_client_config_t http_cfg = {};
  http_cfg.url = url;
  http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  http_cfg.timeout_ms = 20000;

  esp_err_t err = esp_https_ota(&http_cfg);
  if (err == ESP_OK) {
    Serial.println("[ota] success - restarting");
    network_publish_ota_status("success");
    delay(500);   // give the publish above a moment to actually reach the broker before we cut power
    esp_restart();
  } else {
    char msg[48];
    snprintf(msg, sizeof(msg), "error:%s", esp_err_to_name(err));
    Serial.printf("[ota] failed: %s\n", esp_err_to_name(err));
    network_publish_ota_status(msg);
    // No restart, no partial state - the board just keeps running the
    // firmware it already had. esp_https_ota() only switches the boot
    // partition over on a fully verified successful download, so a failed
    // attempt here can't leave the board stuck on a half-written image.
  }
}

void ota_poll(unsigned long now, bool mqttConnected) {
  if (!mqttConnected) return;   // manifest/firmware fetch needs a real network path; MQTT-connected is a reasonable proxy for "WiFi is actually up"
  bool due = s_checkRequested || now >= s_nextCheckAt;
  if (!due) return;

  s_checkRequested = false;
  // Not rescheduled to now + OTA_CHECK_INTERVAL_MS - see ota_init()'s
  // comment. A manual TOPIC_OTA_TRIGGER publish is the only thing that can
  // make another check "due" while this stays disabled.

  network_publish_ota_status("checking");
  char version[32];
  char url[256];
  if (!fetchManifest(version, sizeof(version), url, sizeof(url))) {
    Serial.println("[ota] manifest fetch failed");
    network_publish_ota_status("error:manifest_fetch_failed");
    return;
  }

  if (strcmp(version, FIRMWARE_VERSION) == 0) {
    Serial.println("[ota] up to date");
    network_publish_ota_status("up_to_date");
    return;
  }

  Serial.printf("[ota] new version available: %s (current %s)\n", version, FIRMWARE_VERSION);
  performUpdate(url);
}

#else  // !OTA_ENABLED

// Stub build: no esp_http_client/esp_https_ota/mbedTLS cert bundle linked
// in at all - see config.h's OTA_ENABLED comment for why. network_task.cpp
// calls these unconditionally, so the signatures have to stay, they just do
// nothing now.
#include <Arduino.h>

void ota_request_check() {}
void ota_init() { Serial.println("[ota] disabled (stripped from this build)"); }
void ota_poll(unsigned long now, bool mqttConnected) { (void)now; (void)mqttConnected; }

#endif  // OTA_ENABLED

#pragma once

// WiFi + MQTT credentials live in secrets.h (gitignored, never committed -
// see secrets.h.example for the template). This repo is public (used for
// GitHub-pull OTA - see the OTA section below), so nothing credential-shaped
// can live directly in this file.
#include "secrets.h"

// ================= MQTT =================
// PRODUCTION: HiveMQ Cloud (free "Serverless" tier), TLS required.
#define MQTT_USE_TLS 1                  // 1 = mqtts:// (needs MQTT_ROOT_CA below), 0 = plain mqtt://

// Root CA that signed the broker's certificate. HiveMQ Cloud's TLS
// certificates chain to ISRG Root X1 (Let's Encrypt) - confirmed via
// HiveMQ's own community docs. A self-hosted Mosquitto with a self-signed
// cert would need ITS OWN CA cert here instead.
// [[maybe_unused]]: this header is included by several .cpp files that
// don't reference the cert (only network_task.cpp does) - without this,
// each of those TUs would warn about an unused static.
[[maybe_unused]] static const char *MQTT_ROOT_CA =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

#define DEVICE_ID "esp32monitor"

// ================= OTA (GitHub-pull) =================
// The board is going in permanently at one physical location, so USB
// reflashing won't be practical after that - this lets it pull new
// firmware over WiFi from a GitHub repo instead.
//
// ota/manifest.json and ota/firmware.bin are published automatically by
// .github/workflows/firmware-ota.yml - it builds on every push to main that
// touches src/**, platformio.ini, or the partition/sdkconfig files, then
// commits the built .bin and a manifest carrying FIRMWARE_VERSION back into
// the ota/ folder on main. Nothing to run by hand, but that workflow does
// need six repo secrets set (Settings > Secrets and variables > Actions):
// WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD -
// matching secrets.h. Missing/misnamed secrets don't fail loudly; they just
// come through as empty, which shows up as a compile error deep in
// mqttInit() instead of anything obviously secret-shaped.
//
// TO SHIP AN UPDATE: bump FIRMWARE_VERSION below and push to main. CI does
// the rest. The board checks on boot, every OTA_CHECK_INTERVAL_MS, and
// instantly if you publish to TOPIC_OTA_TRIGGER - no USB needed.
#define FIRMWARE_VERSION "1.1.0"
// Real esp_https_ota implementation is back in the build as of 2026-08-02 -
// automatic boot/interval checks stay off for now (see ota_update.cpp's
// ota_init()) pending the documented MQTT-TLS collision, but manual-trigger
// OTA (publish to TOPIC_OTA_TRIGGER) is live. Flip back to 0 only if flash
// size becomes tight again (this pulls in esp_http_client/esp_https_ota/the
// mbedTLS cert bundle) - see hardware_io.cpp for the relay boot-flicker fix
// that made the earlier size-shaving attempt here unnecessary.
#define OTA_ENABLED 1
#define OTA_MANIFEST_URL "https://raw.githubusercontent.com/Bagyaprem/machinecontrol/main/ota/manifest.json"
#define OTA_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)   // 6 hours

// ---- Topics ----
#define TOPIC_LED_SET         "esp32/" DEVICE_ID "/led/set"
#define TOPIC_LED_STATE       "esp32/" DEVICE_ID "/led/state"
#define TOPIC_PUMP_SET        "esp32/" DEVICE_ID "/pump/set"
#define TOPIC_PUMP_STATE      "esp32/" DEVICE_ID "/pump/state"
#define TOPIC_MIST_SET        "esp32/" DEVICE_ID "/mist/set"
#define TOPIC_MIST_STATE      "esp32/" DEVICE_ID "/mist/state"
#define TOPIC_SOLENOID_SET    "esp32/" DEVICE_ID "/solenoid/set"
#define TOPIC_SOLENOID_STATE  "esp32/" DEVICE_ID "/solenoid/state"
#define TOPIC_SOLENOID_ERROR  "esp32/" DEVICE_ID "/solenoid/error"
#define TOPIC_MOTOR_SET       "esp32/" DEVICE_ID "/motor/set"
#define TOPIC_MOTOR_STATE     "esp32/" DEVICE_ID "/motor/state"
// Backend-owned external PM trigger (see Prompt 2): the backend polls the
// third-party PM API, applies its OWN hysteresis threshold, and publishes
// the already-resolved decision here as {"request": true|false} - the ESP32
// does no threshold math on this value, it just trusts MQTT. This is what
// keeps the firmware free of any third-party API dependency. Retained, so a
// reconnecting board picks up the current external condition immediately
// instead of waiting up to a full poll interval for the next update.
#define TOPIC_MOTOR_EXTERNAL_TRIGGER "esp32/" DEVICE_ID "/motor/external_trigger"
// Raw pm25 value from the same backend-owned external source, for TFT
// DISPLAY ONLY - never used in any control decision (that's still entirely
// TOPIC_MOTOR_EXTERNAL_TRIGGER's resolved boolean above). Retained, so the
// TFT shows the last known reading immediately after a reconnect/reboot
// instead of blank/zero until the next backend poll.
#define TOPIC_EXTERNAL_PM_READING "esp32/" DEVICE_ID "/external_pm/reading"
#define TOPIC_MQ135_READING   "esp32/" DEVICE_ID "/mq135/reading"
// Master on/off schedule - {"enabled":bool,"on_time":"HH:MM","off_time":"HH:MM"}.
// When enabled, the board forces every relay + the motor off the instant
// local time crosses off_time, and leaves things alone (whatever a user
// last set) the instant it crosses on_time - see device_schedule.cpp.
#define TOPIC_SCHEDULE_SET    "esp32/" DEVICE_ID "/schedule/set"
#define TOPIC_SCHEDULE_STATE  "esp32/" DEVICE_ID "/schedule/state"
#define TOPIC_STATUS          "esp32/" DEVICE_ID "/status"
// Publish anything to this topic to force an immediate OTA check (instead
// of waiting for the next OTA_CHECK_INTERVAL_MS) - not retained, it's a
// one-shot trigger, not a state.
#define TOPIC_OTA_TRIGGER     "esp32/" DEVICE_ID "/ota/trigger"
// Retained: idle|checking|up_to_date|updating|success|error:<msg> - lets the
// dashboard show OTA progress instead of the board just silently rebooting.
#define TOPIC_OTA_STATUS      "esp32/" DEVICE_ID "/ota/status"

// ---- Pin map ----
// Reused from the existing esp32-control (Firebase) firmware's confirmed
// hardware wiring - same physical board, relays, and motor driver.
#define PIN_RPWM        25   // motor driver forward PWM
#define PIN_LPWM        26   // motor driver reverse PWM (unused - single direction only)
#define PIN_MOTOR_EN    27   // motor driver enable
// All 4 relays now on one contiguous GPIO16-19 block (2026-07-29): none of
// these are strapping pins, the 32kHz crystal pins (GPIO32/33 - confirmed
// on hardware to float less predictably at boot, causing a relay click
// before firmware takes over), or input-only pins. TFT_DC/TFT_SCK moved to
// GPIO13/14 (former relay pins) to make room - equally safe for the TFT,
// software SPI doesn't care which ordinary GPIOs it uses.
#define PIN_RELAY_LED   16   // relay 1 - LED strip (silkscreen: RX2)
#define PIN_RELAY_MIST  17   // relay 2 - mist diffuser (silkscreen: TX2)
#define PIN_RELAY_PUMP  18   // relay 3 - air pump (silkscreen: D18)
#define PIN_RELAY_VALVE 19   // relay 4 - solenoid / drain valve (silkscreen: D19)
#define PIN_WIFI_LED     2   // onboard LED - WiFi/MQTT status
#define PIN_MQ135       34   // MQ135 AO through an external 15k+15k divider (input-only ADC1 pin) - see device_motor.cpp's readMq135Ppm() ×2 compensation

#define TFT_CS   5
#define TFT_RST  4
#define TFT_DC   13   // moved from GPIO16 to make room for PIN_RELAY_LED above (silkscreen: D13, left row)
#define TFT_MOSI 23
#define TFT_SCK  14   // moved from GPIO17 to make room for PIN_RELAY_MIST above (silkscreen: D14, left row)
#define TFT_LED  21   // backlight

// Per-relay polarity (modules are mixed): false = active-HIGH, true =
// active-LOW - confirmed on hardware, do not change without re-testing.
#define RELAY_PUMP_ACTIVE_LOW   true
#define RELAY_VALVE_ACTIVE_LOW  true
#define RELAY_LED_ACTIVE_LOW    true
#define RELAY_MIST_ACTIVE_LOW   true

// ---- Motor PWM (LEDC) ----
#define PWM_FREQ 15000   // 15 kHz - above audible whine
#define PWM_RES     8    // 8-bit: duty 0-255
#define CH_RPWM     0
#define CH_LPWM     1

// ---- NTP (wall-clock time for the on/off schedule) ----
// This board has no RTC battery, so wall-clock time only exists after a
// successful NTP sync over WiFi (see network_task.cpp's configTime() call).
// device_schedule.cpp refuses to enforce the schedule until synced, rather
// than guessing a time from millis()-since-boot.
#define NTP_SERVER      "pool.ntp.org"
#define TZ_OFFSET_SEC   (5 * 3600 + 30 * 60)   // IST (UTC+5:30) - change if this board is deployed outside India

// ---- MQ135 clean-air calibration ----
// Recalibrate per physical unit: run in fresh outdoor air and tune
// MQ135_R0_KOHMS until the ppm estimate settles near ~400 there.
#define MQ135_RL_KOHMS 10.0
#define MQ135_R0_KOHMS 76.63
// The MQ135 module's own supply voltage (5V - standard for these breakouts,
// and the reason the 15k+15k divider above exists at all: its AO swing would
// otherwise exceed the ESP32 ADC's 3.3V rating). NOT the same number as the
// ESP32's 3.3V ADC reference used to convert raw counts to volts in
// readMq135Ppm() - those are two different voltages that happen to both show
// up in that function; keep them as separate constants so they don't get
// collapsed into one "3.3" again.
#define MQ135_CIRCUIT_VOLTAGE_V 5.0

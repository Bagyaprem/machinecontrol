#pragma once

// Runs pinned to Core 0. Owns WiFi + MQTT entirely - nothing in here ever
// touches a relay/PWM/TFT pin, and nothing on Core 1 ever calls into this
// file or blocks on anything it does.
void networkTaskFn(void *pv);

// This file is the only one that talks directly to the MQTT client handle -
// ota_update.cpp calls this instead of touching esp_mqtt_client_publish
// itself, so that invariant still holds even though OTA also needs to
// publish. No-ops silently if MQTT isn't currently connected (same as every
// other publish path here - the retained cache + reconnect resync is the
// recovery path, not a resend buffer).
void network_publish_ota_status(const char *status);

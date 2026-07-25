#pragma once

// GitHub-pull OTA. Runs entirely on Core 0 (called from network_task.cpp) -
// it's a network operation, never touches a relay/PWM pin, and never runs on
// Core 1. See config.h's OTA section for one-time GitHub setup.

void ota_init();

// Called once per networkTaskFn loop tick (cheap no-op unless a check is
// actually due or was just requested).
void ota_poll(unsigned long now, bool mqttConnected);

// Set by network_task.cpp's MQTT handler when TOPIC_OTA_TRIGGER arrives -
// makes the next ota_poll() run a check immediately instead of waiting for
// OTA_CHECK_INTERVAL_MS.
void ota_request_check();

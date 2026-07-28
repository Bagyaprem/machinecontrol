#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Cross-core communication is queue-only, in both directions - this is what
// lets Core 1's device logic never call into anything network-related and
// Core 0's network task never touch a relay/PWM pin directly.
//
//   Core 0 (network) --cmdQueueHandle-->   Core 1 (devices)
//   Core 0 (network) <--stateQueueHandle-- Core 1 (devices)
//
// Both queues are drained non-blocking (0 tick timeout) from whichever side
// consumes them, so a full/empty queue never stalls either core.

enum CommandType {
  CMD_LED_SET,
  CMD_PUMP_SET,
  CMD_MIST_PULSE,        // intValue = 1/2/3 (seconds)
  CMD_MIST_AUTO_SET,     // boolValue = enable/disable auto-cycle
  CMD_SOLENOID_PULSE,    // no payload
  CMD_SOLENOID_HOLD_SET, // boolValue = hold on/off
  CMD_MOTOR_SET,             // intValue = 0/50/75/100
  CMD_MOTOR_AUTO_SET,        // boolValue = enable/disable the mq135/external-PM auto-trigger switch
  CMD_EXTERNAL_TRIGGER_SET,  // boolValue = backend-resolved external-PM request (hysteresis already applied by the backend)
  CMD_EXTERNAL_READING_SET,  // floatValue = raw pm25 for TFT display only, never a control input
  CMD_SCHEDULE_SET,          // schedule = {enabled, onH, onM, offH, offM}
};

struct Command {
  CommandType type;
  union {
    bool boolValue;
    int intValue;
    float floatValue;
    struct { bool enabled; uint8_t onH, onM, offH, offM; } schedule;
  };
};

enum StateTopicId {
  STATE_LED,
  STATE_PUMP,
  STATE_MIST,
  STATE_SOLENOID,
  STATE_SOLENOID_ERROR,   // one-shot conflict report, not retained
  STATE_MOTOR,
  STATE_MQ135_READING,    // one-shot sensor reading, not retained
  STATE_SCHEDULE,
};

struct StateUpdate {
  StateTopicId topic;
  char json[128];   // pre-serialized on Core 1 so Core 0 never touches JsonDocument/heap owned by the other core
};

#define CMD_QUEUE_LEN   16
#define STATE_QUEUE_LEN 16

extern QueueHandle_t cmdQueueHandle;
extern QueueHandle_t stateQueueHandle;

// Written only by the network task, read-only everywhere else (e.g. the TFT
// display). A single bool is small enough to treat as atomic for a
// display/telemetry flag - Core 1 never blocks waiting on it.
extern volatile bool g_mqttConnected;

// Same pattern as g_mqttConnected, but tracks WiFi association specifically
// (set in network_task.cpp's wifiSupervisor()) - used by main.cpp's boot
// restore gate to know when it's safe to apply persisted relay/motor state.
extern volatile bool g_wifiConnected;

void state_bus_init();

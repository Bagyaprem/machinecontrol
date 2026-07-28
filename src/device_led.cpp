#include "device_led.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <ArduinoJson.h>

static bool ledOn = false;
static bool dirty = true;   // forces one initial publish so a fresh dashboard load sees a value immediately

static bool pendingRestore = false;

void led_init() {
  // Relay stays OFF (already set by hardware_io_init()) until
  // led_apply_restored_state() runs - see main.cpp's boot restore gate.
  pendingRestore = persist_get_bool("led", false);
  dirty = true;
}

void led_apply_restored_state() {
  ledOn = pendingRestore;
  hw_write_led(ledOn);
  dirty = true;
}

void led_set(bool on) {
  if (on == ledOn) return;
  ledOn = on;
  hw_write_led(ledOn);   // sole writer of this relay's pin
  persist_set_bool("led", ledOn);
  dirty = true;
}

bool led_get_on() { return ledOn; }

void led_update() {
  if (!dirty) return;
  dirty = false;
  StateUpdate upd;
  upd.topic = STATE_LED;
  JsonDocument doc;
  doc["on"] = ledOn;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

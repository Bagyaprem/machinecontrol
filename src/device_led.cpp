#include "device_led.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <ArduinoJson.h>

static bool ledOn = false;
static bool dirty = true;   // forces one initial publish so a fresh dashboard load sees a value immediately

void led_init() {
  ledOn = persist_get_bool("led", false);   // power-cut restore: come back exactly as last commanded
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

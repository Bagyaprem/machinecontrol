#include "device_pump.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <ArduinoJson.h>

static bool pumpOn = false;
static bool dirty = true;

void pump_init() {
  pumpOn = persist_get_bool("pump", false);   // power-cut restore: come back exactly as last commanded
  hw_write_pump(pumpOn);
  dirty = true;
}

void pump_set(bool on) {
  if (on == pumpOn) return;
  pumpOn = on;
  hw_write_pump(pumpOn);   // sole writer of this relay's pin
  persist_set_bool("pump", pumpOn);
  dirty = true;
}

bool pump_get_on() { return pumpOn; }

void pump_update() {
  if (!dirty) return;
  dirty = false;
  StateUpdate upd;
  upd.topic = STATE_PUMP;
  JsonDocument doc;
  doc["on"] = pumpOn;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

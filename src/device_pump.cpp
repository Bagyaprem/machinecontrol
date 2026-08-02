#include "device_pump.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <ArduinoJson.h>

static bool pumpOn = false;
static bool dirty = true;

static bool pendingRestore = false;
// Cancelled by any manual command that arrives before the boot-restore gate
// fires - see the matching comment in device_led.cpp.
static bool restorePending = true;

void pump_init() {
  // Relay stays OFF (already set by hardware_io_init()) until
  // pump_apply_restored_state() runs - see main.cpp's boot restore gate.
  pendingRestore = persist_get_bool("pump", false);
  dirty = true;
}

void pump_apply_restored_state() {
  if (!restorePending) return;   // a manual command already arrived - don't clobber it
  pumpOn = pendingRestore;
  hw_write_pump(pumpOn);
  dirty = true;
}

void pump_set(bool on) {
  restorePending = false;
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

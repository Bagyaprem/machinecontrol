#include "device_solenoid.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <Arduino.h>
#include <ArduinoJson.h>

#define SOLENOID_PULSE_MS 300UL

// DESIGN DECISION: pulse mode and hold mode are mutually exclusive, and any
// conflict between them is REJECTED, never silently auto-resolved:
//   - a pulse request while hold is ON is rejected: hold stays ON, no pulse
//     starts, and the conflict is reported on a dedicated error topic
//     (TOPIC_SOLENOID_ERROR) rather than folded into /state.
//   - a hold-ON request while a pulse is running is rejected the same way.
//   - a hold-OFF request while a pulse is running is accepted as a no-op
//     (hold was never on, so there's nothing to change) - not a conflict.
//   - a second pulse request while already pulsing is ignored, same
//     ignore-not-queue choice as the mist diffuser.
// Rejecting keeps the relay's state fully determined by the last ACCEPTED
// command alone - there's no implicit "cancel hold and pulse anyway" step
// that could leave the two modes' bookkeeping out of sync with each other.

static bool holdOn = false;
static bool pulsing = false;
static unsigned long pulseStartMs = 0;
static bool dirty = true;

// Sole writer of the solenoid relay pin.
static void applyRelay(bool on) {
  hw_write_solenoid(on);
}

static void reportConflict(const char *reason) {
  StateUpdate upd;
  upd.topic = STATE_SOLENOID_ERROR;
  JsonDocument doc;
  doc["error"] = reason;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

void solenoid_init() {
  holdOn = persist_get_bool("sol_hold", false);   // power-cut restore: sustained hold only, never a pulse-in-flight
  applyRelay(holdOn);
  dirty = true;
}

void solenoid_trigger_pulse() {
  if (holdOn) { reportConflict("pulse_rejected_hold_active"); return; }
  if (pulsing) return;   // ignored - already timing
  pulsing = true;
  pulseStartMs = millis();
  applyRelay(true);
  dirty = true;
}

void solenoid_set_hold(bool on) {
  if (on == holdOn) return;
  if (on && pulsing) { reportConflict("hold_on_rejected_pulse_active"); return; }
  holdOn = on;
  if (holdOn) {
    applyRelay(true);
  } else if (!pulsing) {
    applyRelay(false);
  }
  persist_set_bool("sol_hold", holdOn);
  dirty = true;
}

bool solenoid_get_hold() { return holdOn; }
bool solenoid_get_pulsing() { return pulsing; }

void solenoid_update() {
  if (pulsing && millis() - pulseStartMs >= SOLENOID_PULSE_MS) {
    pulsing = false;
    if (!holdOn) applyRelay(false);   // holdOn can't actually be true here (hold-ON is rejected while pulsing), kept as a defensive guard
    dirty = true;
  }

  if (!dirty) return;
  dirty = false;
  StateUpdate upd;
  upd.topic = STATE_SOLENOID;
  JsonDocument doc;
  doc["hold"] = holdOn;
  doc["pulsing"] = pulsing;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

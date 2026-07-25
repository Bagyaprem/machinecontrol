#include "device_mist.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "persist.h"
#include <Arduino.h>
#include <ArduinoJson.h>

// Mist diffuser state machine.
//
//   manual trigger ---> IDLE --------> PULSING --------> IDLE
//                                (relay HIGH, timer      (relay LOW)
//                                 for 1/2/3 s, caller's choice)
//
//   auto-cycle     ---> CYCLE_WAITING -> PULSING -> CYCLE_WAITING
//                        (waiting out    (reuses the exact same pulse
//                         the 30-min     mechanism as manual, fixed 3s)
//                         interval)
//
// `pulsing` + `activeSource` together ARE both sub-machines: there is only
// ever one physical pulse in flight, and activeSource records which trigger
// owns it. That single shared pulse mechanism is what "mode" mutual
// exclusion actually means here - manual and auto can't both be mid-pulse
// at once because there's only one `pulsing` flag and one relay.
//
// DESIGN DECISION: a manual trigger received while already pulsing is
// IGNORED, not queued. Pulses are short (1-3s) and a dropped duplicate
// button press is simpler and safer to reason about than a queued backlog
// of presses firing back-to-back later.
//
// Auto-cycle interval is 30 minutes (matches the real hardware's intended
// cadence, per esp32-control's original firmware and the user's explicit
// confirmation) - NOT the 60s used during initial spec drafting.
#define MIST_AUTO_CYCLE_MS  (30UL * 60UL * 1000UL)
#define MIST_AUTO_PULSE_MS  3000UL

enum MistSource { MIST_SRC_NONE, MIST_SRC_MANUAL, MIST_SRC_AUTO };

static bool pulsing = false;
static MistSource activeSource = MIST_SRC_NONE;
static unsigned long pulseStartMs = 0;
static unsigned long pulseDurationMs = 0;

static bool autoEnabled = false;
static unsigned long nextAutoCycleAt = 0;

static bool dirty = true;

// Sole writer of the mist relay pin - both the pulse-start and pulse-expiry
// paths in this file call ONLY through here.
static bool startPulse(MistSource src, unsigned long durationMs) {
  if (pulsing) return false;   // ignored - see design-decision comment above
  pulsing = true;
  activeSource = src;
  pulseDurationMs = durationMs;
  pulseStartMs = millis();
  hw_write_mist(true);
  dirty = true;
  return true;
}

void mist_init() {
  hw_write_mist(false);   // never restore a pulse-in-flight - only the schedule flag below
  autoEnabled = persist_get_bool("mist_auto", false);   // power-cut restore
  if (autoEnabled) nextAutoCycleAt = millis() + MIST_AUTO_CYCLE_MS;   // fresh countdown from boot
  dirty = true;
}

void mist_trigger_manual(int seconds) {
  if (seconds != 1 && seconds != 2 && seconds != 3) return;
  startPulse(MIST_SRC_MANUAL, (unsigned long)seconds * 1000UL);
}

void mist_set_auto(bool enabled) {
  if (enabled == autoEnabled) return;
  autoEnabled = enabled;
  if (autoEnabled) {
    nextAutoCycleAt = millis() + MIST_AUTO_CYCLE_MS;   // fresh 30-min countdown from the moment it's enabled
  }
  persist_set_bool("mist_auto", autoEnabled);
  dirty = true;
}

bool mist_get_pulsing() { return pulsing; }
bool mist_get_auto() { return autoEnabled; }

void mist_update() {
  unsigned long now = millis();

  if (pulsing && (now - pulseStartMs >= pulseDurationMs)) {
    pulsing = false;
    activeSource = MIST_SRC_NONE;
    hw_write_mist(false);   // sole writer, expiry path
    dirty = true;
  }

  // Auto-cycle: fires every 30 minutes while enabled, reusing the same pulse
  // mechanism as manual (1s pulse). If a manual pulse happens to be running
  // right when a cycle is due, startPulse() no-ops and returns false here -
  // nextAutoCycleAt is deliberately left in the past in that case (instead
  // of being bumped to now+MIST_AUTO_CYCLE_MS), so this check retries every loop tick
  // (cheap - just a comparison) until the manual pulse clears, then fires
  // immediately. That's what keeps the auto-cycle from silently skipping a
  // cycle on a rare collision with a manual trigger.
  if (autoEnabled && now >= nextAutoCycleAt) {
    if (startPulse(MIST_SRC_AUTO, MIST_AUTO_PULSE_MS)) {
      nextAutoCycleAt = now + MIST_AUTO_CYCLE_MS;
    }
  }

  if (!dirty) return;
  dirty = false;
  StateUpdate upd;
  upd.topic = STATE_MIST;
  JsonDocument doc;
  doc["pulsing"] = pulsing;
  doc["auto_enabled"] = autoEnabled;
  doc["source"] = activeSource == MIST_SRC_MANUAL ? "manual" : activeSource == MIST_SRC_AUTO ? "auto" : "none";
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

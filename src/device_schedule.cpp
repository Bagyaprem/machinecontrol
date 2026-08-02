#include "device_schedule.h"
#include "state_bus.h"
#include "persist.h"
#include "device_led.h"
#include "device_pump.h"
#include "device_mist.h"
#include "device_solenoid.h"
#include "device_motor.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// Master on/off schedule: a daily time-of-day window during which the
// machine is allowed to run. on_time > off_time is valid and means the
// window wraps past midnight (e.g. on=22:00/off=06:00 runs overnight).
//
// Time source is NTP (network_task.cpp calls configTime() at boot) - this
// board has no RTC battery, so "now" is unknown until the first successful
// sync. Enforcement is skipped entirely until then; it never guesses a
// time from millis()-since-boot, which has no relationship to the wall
// clock.
//
// DESIGN: only the CLOSING edge (in-window -> out-of-window) forces every
// relay + the motor off, by calling each device's own public setter (the
// same sole-writer entry point a manual MQTT command would use). The
// OPENING edge deliberately does nothing - whatever a user commands during
// the closed window (or the state from just before closing) is left alone.
// This is a closing gate, not a "resume to some remembered state" feature -
// consistent with how the motor's own overrideOff auto-clears on a fresh
// manual command elsewhere in this firmware, rather than replaying history.

static bool s_enabled = false;
static int s_onH, s_onM, s_offH, s_offM;
static int s_lastInWindow = -1;   // -1 = not yet evaluated since boot/enable
static bool s_timeSynced = false;
static bool s_dirty = true;

static bool computeInWindow(int curMin, int onMin, int offMin) {
  if (onMin == offMin) return true;   // degenerate: schedule spans the full day
  if (onMin < offMin) return curMin >= onMin && curMin < offMin;
  return curMin >= onMin || curMin < offMin;   // wraps past midnight
}

static void forceAllOff() {
  led_set(false);
  pump_set(false);
  mist_set_auto(false);
  solenoid_set_hold(false);
  motor_set_manual(0);
  // Zeroing manual_speed alone is not enough: if auto mode is on and a
  // trigger (mq135/external PM) is still latched true, motor_update()'s
  // resolver would recompute requested=100/50 from that trigger on the very
  // next tick and re-energize the motor inside the window this function
  // exists to close. Disabling auto (mirroring mist_set_auto above) removes
  // that path entirely, same as a manual command would.
  motor_set_auto(false);
}

void schedule_init() {
  s_enabled = persist_get_bool("sch_en", false);
  s_onH  = persist_get_int("sch_on_h", 8);
  s_onM  = persist_get_int("sch_on_m", 0);
  s_offH = persist_get_int("sch_off_h", 20);
  s_offM = persist_get_int("sch_off_m", 0);
  s_dirty = true;
}

void schedule_set(bool enabled, int onH, int onM, int offH, int offM) {
  s_enabled = enabled;
  s_onH = onH; s_onM = onM; s_offH = offH; s_offM = offM;
  s_lastInWindow = -1;   // force a fresh (re-)evaluation on the next update()
  persist_set_bool("sch_en", s_enabled);
  persist_set_int("sch_on_h", s_onH);
  persist_set_int("sch_on_m", s_onM);
  persist_set_int("sch_off_h", s_offH);
  persist_set_int("sch_off_m", s_offM);
  s_dirty = true;
}

void schedule_update() {
  static unsigned long lastClockCheckAt = 0;
  unsigned long now = millis();

  if (now - lastClockCheckAt >= 1000UL) {
    lastClockCheckAt = now;

    time_t t = time(nullptr);
    struct tm tmNow;
    localtime_r(&t, &tmNow);
    bool synced = (tmNow.tm_year + 1900) >= 2024;
    if (synced != s_timeSynced) { s_timeSynced = synced; s_dirty = true; }

    if (s_enabled && synced) {
      int curMin = tmNow.tm_hour * 60 + tmNow.tm_min;
      int onMin  = s_onH * 60 + s_onM;
      int offMin = s_offH * 60 + s_offM;
      bool inWindow = computeInWindow(curMin, onMin, offMin);
      int inWindowInt = inWindow ? 1 : 0;
      if (inWindowInt != s_lastInWindow) {
        if (!inWindow) forceAllOff();   // closing edge only - see design note above
        s_lastInWindow = inWindowInt;
        s_dirty = true;
      }
    } else if (!s_enabled) {
      s_lastInWindow = -1;
    }
  }

  if (!s_dirty) return;
  s_dirty = false;
  StateUpdate upd;
  upd.topic = STATE_SCHEDULE;
  JsonDocument doc;
  doc["enabled"] = s_enabled;
  char onBuf[6], offBuf[6];
  snprintf(onBuf, sizeof(onBuf), "%02d:%02d", s_onH, s_onM);
  snprintf(offBuf, sizeof(offBuf), "%02d:%02d", s_offH, s_offM);
  doc["on_time"] = onBuf;
  doc["off_time"] = offBuf;
  doc["time_synced"] = s_timeSynced;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

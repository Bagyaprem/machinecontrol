#include "device_motor.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "config.h"
#include "persist.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

// Priority resolver (updated again 2026-07-29 per explicit user request):
// a separate master "auto" switch now gates whether the MQ135/external-PM
// triggers get to influence the speed at all - mirrors the mist module's
// own auto_enabled toggle. manual_speed is ALWAYS applied regardless of
// this switch ("manual check" - lets you directly verify/drive the motor
// at any time, auto on or off).
//
//   requested = autoEnabled ? MAX(manual_speed, mq135_request?100:0, external_pm_request?50:0)
//                           : manual_speed
//
// With auto OFF (the default, and the previous behavior this replaces):
// the motor is purely manual, full stop, regardless of ppm/pm2.5.
// With auto ON: it follows CO2/PM2.5 automatically, boosting above
// whatever manual_speed is currently set to - but manual_speed itself
// still works normally the whole time, so you can always manually check/
// drive it to a specific speed without having to first disable auto.

#define MQ135_POLL_MS 3000UL

// Hysteresis - ON strictly above OFF - so the request flag can't chatter
// right at a single boundary value. This applies ONLY to the local MQ135
// reading: the external-PM request arrives pre-thresholded from the
// backend (see motor_set_external_trigger below and config.h's
// TOPIC_MOTOR_EXTERNAL_TRIGGER comment) - the backend's hysteresis
// constants live in that service, not here, so they can be tuned/deployed
// without reflashing this board.
#define MQ135_ON_PPM     100.0f
#define MQ135_OFF_PPM     80.0f

static int manualSpeed = 0;
static bool autoEnabled = false;
static bool mq135Request = false;
static bool externalPmRequest = false;
static int appliedSpeed = -1;   // -1 forces the very first apply/publish
static bool dirty = true;

static unsigned long lastMq135PollAt = 0;
static float lastMq135Ppm = 0;
static int lastMq135Raw = 0;

// Returns NAN when the reading is unusable - callers MUST check isfinite()
// before using it for anything. See the rs guard below.
static float readMq135Ppm() {
  int raw = analogRead(PIN_MQ135);
  lastMq135Raw = raw;
  // ×2: a 1:1 (15k+15k) voltage divider sits between the MQ135's AO pin and
  // this GPIO now, to keep the ADC input within its 3.3V rating - see
  // config.h's pin map comment. Undo the halving here before the Rs math.
  float voltage = raw * (3.3f / 4095.0f) * 2.0f;   // 3.3f here is the ESP32 ADC reference - unrelated to the sensor's own supply voltage below
  if (voltage < 0.01f) voltage = 0.01f;   // guard divide-by-zero if the sensor's unplugged/shorted
  // Was hardcoded 3.3f here (leftover from before the divider existed, when
  // the sensor's AO went straight into the 3.3V ADC and the two voltages
  // were coincidentally the same number) - see config.h's
  // MQ135_CIRCUIT_VOLTAGE_V comment for why that's wrong now that the module
  // runs on 5V with a divider bringing its signal back down to ADC-safe
  // range. Left uncorrected, this skews every reading except right at the
  // single point MQ135_R0_KOHMS was field-calibrated against.
  float rs = ((MQ135_CIRCUIT_VOLTAGE_V * MQ135_RL_KOHMS) / voltage) - MQ135_RL_KOHMS;

  // rs goes NEGATIVE as soon as the compensated voltage exceeds 3.3V, i.e.
  // raw > ~2047. powf(negative, -2.769) is NaN - and NaN then fails BOTH
  // hysteresis comparisons in pollMq135() (NaN >= ON and NaN <= OFF are each
  // false), which used to freeze mq135Request forever with nothing logged
  // anywhere. Observed live on this board: a railed sensor (raw=4095) published
  // {"raw":4095,"ppm":null} and silently disabled the CO2 auto-trigger
  // completely. Fail loudly-but-safely instead: NAN out, caller forces the
  // request off and marks the reading invalid.
  if (rs <= 0.0f) return NAN;

  float ratio = rs / MQ135_R0_KOHMS;
  float ppm = 116.6020682f * powf(ratio, -2.769034857f);   // standard MQ135 CO2-equivalent curve fit
  return isfinite(ppm) ? ppm : NAN;
}

static int pendingSpeedRestore = 0;
// Cancelled by a manual speed command that arrives before the boot-restore
// gate fires - see the matching comment in device_led.cpp. auto_enabled
// isn't covered by this flag: it's restored immediately in motor_init()
// (see comment below), not gated behind bootRestoreGate() at all.
static bool restorePending = true;

void motor_init() {
  hw_write_motor_pwm(0);
  // Motor stays at 0 (manualSpeed default) until motor_apply_restored_state()
  // runs - see main.cpp's boot restore gate. The trigger flags (mq135/
  // external) are live inputs re-evaluated from scratch after boot, so only
  // the last MANUAL speed needs restoring, same as before.
  int saved = persist_get_int("motor_spd", 0);
  if (saved == 0 || saved == 50 || saved == 75 || saved == 100) pendingSpeedRestore = saved;
  // auto_enabled restored immediately - it doesn't energize anything by
  // itself, it only permits a FUTURE mq135/PM trigger to raise the speed.
  autoEnabled = persist_get_bool("motor_auto", false);
  lastMq135PollAt = millis();
}

void motor_apply_restored_state() {
  if (!restorePending) return;   // a manual command already arrived - don't clobber it
  manualSpeed = pendingSpeedRestore;
  dirty = true;
}

void motor_set_manual(int speed) {
  if (speed != 0 && speed != 50 && speed != 75 && speed != 100) return;
  restorePending = false;
  manualSpeed = speed;
  persist_set_int("motor_spd", manualSpeed);
  dirty = true;
}

void motor_set_auto(bool enabled) {
  if (enabled == autoEnabled) return;
  autoEnabled = enabled;
  persist_set_bool("motor_auto", autoEnabled);
  dirty = true;
}

void motor_set_external_trigger(bool request) {
  // No threshold math here by design - the backend already applied its own
  // hysteresis before deciding to publish this value (see Prompt 2). The
  // firmware trusts MQTT for this input entirely, which is what keeps it
  // free of any third-party API dependency.
  if (request == externalPmRequest) return;
  externalPmRequest = request;
  dirty = true;
}

int motor_get_applied_speed() { return appliedSpeed; }
float motor_get_last_mq135_ppm() { return lastMq135Ppm; }
bool motor_get_air_quality_danger() { return mq135Request || externalPmRequest; }

static void pollMq135(unsigned long now) {
  if (now - lastMq135PollAt < MQ135_POLL_MS) return;
  lastMq135PollAt = now;
  lastMq135Ppm = readMq135Ppm();

  const bool valid = isfinite(lastMq135Ppm);
  if (valid) {
    if (!mq135Request && lastMq135Ppm >= MQ135_ON_PPM) mq135Request = true;
    else if (mq135Request && lastMq135Ppm <= MQ135_OFF_PPM) mq135Request = false;
  } else if (mq135Request) {
    // Unusable reading: force the request OFF rather than leaving it stuck at
    // whatever it last was. A broken/unplugged CO2 sensor must never be able
    // to hold the motor on indefinitely with no way to clear it.
    mq135Request = false;
  }

  StateUpdate reading;
  reading.topic = STATE_MQ135_READING;
  JsonDocument doc;
  doc["raw"] = lastMq135Raw;
  // Explicit flag so a bad sensor is visibly a FAULT downstream, not just a
  // mysterious null that looks like a serialization quirk.
  doc["valid"] = valid;
  if (valid) doc["ppm"] = lastMq135Ppm;
  else       doc["ppm"] = nullptr;
  serializeJson(doc, reading.json, sizeof(reading.json));
  xQueueSend(stateQueueHandle, &reading, 0);

  dirty = true;
}

void motor_update() {
  unsigned long now = millis();
  pollMq135(now);

  // manual_speed always applies ("manual check"); auto-triggers only raise
  // it further, and only while the auto switch is on - see the
  // priority-resolver comment above.
  int requested = manualSpeed;
  if (autoEnabled) {
    if (mq135Request && requested < 100) requested = 100;
    if (externalPmRequest && requested < 50) requested = 50;
  }

  if (requested != appliedSpeed) {
    appliedSpeed = requested;
    hw_write_motor_pwm(appliedSpeed);   // sole writer of the motor PWM pins - called only from here
    dirty = true;
  }

  if (!dirty) return;
  dirty = false;
  StateUpdate upd;
  upd.topic = STATE_MOTOR;
  JsonDocument doc;
  doc["manual_speed"] = manualSpeed;
  doc["applied_speed"] = appliedSpeed;
  doc["auto_enabled"] = autoEnabled;
  doc["mq135_request"] = mq135Request;
  doc["external_pm_request"] = externalPmRequest;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

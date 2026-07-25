#include "device_motor.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "config.h"
#include "persist.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

// Priority resolver (spec):
//   requested = MAX(mq135_request, external_pm_request, manual_request)
//   if (user issued manual OFF while a trigger condition is active):
//       override_off = true
//   if (override_off AND both trigger sources have cleared below their OFF thresholds):
//       override_off = false
//   final_output = override_off ? 0 : requested
//
// DESIGN DECISION (override reset behavior): override_off AUTO-CLEARS once
// BOTH trigger sources have dropped back below their OFF thresholds - it
// does NOT require a fresh explicit manual command to release. This matches
// the spec's pseudocode verbatim (motor_update() below implements exactly
// that line). The alternative - requiring an explicit manual re-enable -
// would mean a single manual OFF permanently silences auto triggers until
// the user acts again, which is arguably safer for some deployments, but is
// not what this spec asked for.
//
// A second, smaller decision made here: an explicit manual command to a
// NON-ZERO speed always clears override_off too, even before both triggers
// clear. Rationale: if the user is actively re-commanding the motor, that's
// a more recent and more specific instruction than the stale override, and
// there's no scenario where "motor stays force-off despite the operator
// just asking for 50%" is the intended behavior.

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
static bool overrideOff = false;
static bool mq135Request = false;
static bool externalPmRequest = false;
static int appliedSpeed = -1;   // -1 forces the very first apply/publish
static bool dirty = true;

static unsigned long lastMq135PollAt = 0;
static float lastMq135Ppm = 0;
static int lastMq135Raw = 0;

static float readMq135Ppm() {
  int raw = analogRead(PIN_MQ135);
  lastMq135Raw = raw;
  float voltage = raw * (3.3f / 4095.0f);
  if (voltage < 0.01f) voltage = 0.01f;   // guard divide-by-zero if the sensor's unplugged/shorted
  float rs = ((3.3f * MQ135_RL_KOHMS) / voltage) - MQ135_RL_KOHMS;
  float ratio = rs / MQ135_R0_KOHMS;
  return 116.6020682f * powf(ratio, -2.769034857f);   // standard MQ135 CO2-equivalent curve fit
}

void motor_init() {
  hw_write_motor_pwm(0);
  // Power-cut restore: only the last MANUAL speed. The trigger flags
  // (mq135/external) are live inputs re-evaluated from scratch after boot,
  // and override_off is transient bookkeeping tied to those flags - neither
  // makes sense to replay. appliedSpeed stays -1 so the first motor_update()
  // tick runs the full priority resolver against the restored manual speed.
  int saved = persist_get_int("motor_spd", 0);
  if (saved == 0 || saved == 50 || saved == 75 || saved == 100) manualSpeed = saved;
  lastMq135PollAt = millis();
}

void motor_set_manual(int speed) {
  if (speed != 0 && speed != 50 && speed != 75 && speed != 100) return;
  manualSpeed = speed;
  if (speed == 0) {
    if (mq135Request || externalPmRequest) overrideOff = true;
  } else {
    overrideOff = false;   // explicit non-zero manual command always clears a stale override - see comment above
  }
  persist_set_int("motor_spd", manualSpeed);
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

  if (!mq135Request && lastMq135Ppm >= MQ135_ON_PPM) mq135Request = true;
  else if (mq135Request && lastMq135Ppm <= MQ135_OFF_PPM) mq135Request = false;

  StateUpdate reading;
  reading.topic = STATE_MQ135_READING;
  JsonDocument doc;
  doc["raw"] = lastMq135Raw;
  doc["ppm"] = lastMq135Ppm;
  serializeJson(doc, reading.json, sizeof(reading.json));
  xQueueSend(stateQueueHandle, &reading, 0);

  dirty = true;
}

void motor_update() {
  unsigned long now = millis();
  pollMq135(now);

  if (overrideOff && !mq135Request && !externalPmRequest) {
    overrideOff = false;
    dirty = true;
  }

  int requested = manualSpeed;
  if (mq135Request && requested < 100) requested = 100;
  if (externalPmRequest && requested < 50) requested = 50;

  int finalOutput = overrideOff ? 0 : requested;

  if (finalOutput != appliedSpeed) {
    appliedSpeed = finalOutput;
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
  doc["mq135_request"] = mq135Request;
  doc["external_pm_request"] = externalPmRequest;
  doc["override_off"] = overrideOff;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

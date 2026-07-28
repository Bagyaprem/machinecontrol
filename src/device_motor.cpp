#include "device_motor.h"
#include "hardware_io.h"
#include "state_bus.h"
#include "config.h"
#include "persist.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

// Priority resolver (updated 2026-07-29 per explicit user request): the
// MQ135/external-PM auto-triggers only ever get to raise the speed - they
// can never turn the motor on by themselves. If the user has it manually
// OFF (manualSpeed == 0), it stays off regardless of ppm/pm2.5, full stop.
// Only once the user has manually turned it on to some speed do the
// triggers get to bump that speed up further (mq135 to at least 100,
// external PM to at least 50).
//
//   requested = manualSpeed == 0 ? 0 : MAX(manual_request, mq135_request, external_pm_request)
//
// This replaces the previous design, where a trigger could autonomously
// start the motor even from a manual OFF state (with an override_off escape
// hatch to suppress that) - that whole override concept is gone now, since
// there's nothing left for it to override.

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
  // ×2: a 1:1 (15k+15k) voltage divider sits between the MQ135's AO pin and
  // this GPIO now, to keep the ADC input within its 3.3V rating - see
  // config.h's pin map comment. Undo the halving here before the Rs math.
  float voltage = raw * (3.3f / 4095.0f) * 2.0f;
  if (voltage < 0.01f) voltage = 0.01f;   // guard divide-by-zero if the sensor's unplugged/shorted
  float rs = ((3.3f * MQ135_RL_KOHMS) / voltage) - MQ135_RL_KOHMS;
  float ratio = rs / MQ135_R0_KOHMS;
  return 116.6020682f * powf(ratio, -2.769034857f);   // standard MQ135 CO2-equivalent curve fit
}

void motor_init() {
  hw_write_motor_pwm(0);
  // Power-cut restore: only the last MANUAL speed. The trigger flags
  // (mq135/external) are live inputs re-evaluated from scratch after boot.
  // appliedSpeed stays -1 so the first motor_update() tick runs the full
  // priority resolver against the restored manual speed.
  int saved = persist_get_int("motor_spd", 0);
  if (saved == 0 || saved == 50 || saved == 75 || saved == 100) manualSpeed = saved;
  lastMq135PollAt = millis();
}

void motor_set_manual(int speed) {
  if (speed != 0 && speed != 50 && speed != 75 && speed != 100) return;
  manualSpeed = speed;
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

  // Auto-triggers only ever raise the speed, and only once the motor is
  // already manually on - see the priority-resolver comment above.
  int requested = manualSpeed;
  if (manualSpeed != 0) {
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
  doc["mq135_request"] = mq135Request;
  doc["external_pm_request"] = externalPmRequest;
  serializeJson(doc, upd.json, sizeof(upd.json));
  xQueueSend(stateQueueHandle, &upd, 0);
}

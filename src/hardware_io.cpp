#include "hardware_io.h"
#include "config.h"
#include <Arduino.h>

static inline void relayWrite(int pin, bool on, bool activeLow) {
  digitalWrite(pin, (activeLow ? !on : on) ? HIGH : LOW);
}

void hardware_io_init() {
  pinMode(PIN_RELAY_LED,   OUTPUT); relayWrite(PIN_RELAY_LED,   false, RELAY_LED_ACTIVE_LOW);
  pinMode(PIN_RELAY_PUMP,  OUTPUT); relayWrite(PIN_RELAY_PUMP,  false, RELAY_PUMP_ACTIVE_LOW);
  pinMode(PIN_RELAY_MIST,  OUTPUT); relayWrite(PIN_RELAY_MIST,  false, RELAY_MIST_ACTIVE_LOW);
  pinMode(PIN_RELAY_VALVE, OUTPUT); relayWrite(PIN_RELAY_VALVE, false, RELAY_VALVE_ACTIVE_LOW);

  pinMode(PIN_MOTOR_EN, OUTPUT); digitalWrite(PIN_MOTOR_EN, LOW);
  ledcSetup(CH_RPWM, PWM_FREQ, PWM_RES); ledcAttachPin(PIN_RPWM, CH_RPWM);
  ledcSetup(CH_LPWM, PWM_FREQ, PWM_RES); ledcAttachPin(PIN_LPWM, CH_LPWM);
  ledcWrite(CH_RPWM, 0);
  ledcWrite(CH_LPWM, 0);
}

void hw_write_led(bool on)      { relayWrite(PIN_RELAY_LED,   on, RELAY_LED_ACTIVE_LOW); }
void hw_write_pump(bool on)     { relayWrite(PIN_RELAY_PUMP,  on, RELAY_PUMP_ACTIVE_LOW); }
void hw_write_mist(bool on)     { relayWrite(PIN_RELAY_MIST,  on, RELAY_MIST_ACTIVE_LOW); }
void hw_write_solenoid(bool on) { relayWrite(PIN_RELAY_VALVE, on, RELAY_VALVE_ACTIVE_LOW); }

void hw_write_motor_pwm(int speedPercent) {
  ledcWrite(CH_LPWM, 0);   // single direction only, matches existing hardware/wiring
  if (speedPercent <= 0) {
    ledcWrite(CH_RPWM, 0);
    digitalWrite(PIN_MOTOR_EN, LOW);   // disable driver when stopped
    return;
  }
  digitalWrite(PIN_MOTOR_EN, HIGH);
  ledcWrite(CH_RPWM, map(speedPercent, 0, 100, 0, 255));
}

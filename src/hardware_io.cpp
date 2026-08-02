#include "hardware_io.h"
#include "config.h"
#include <Arduino.h>

static inline void relayWrite(int pin, bool on, bool activeLow) {
  digitalWrite(pin, (activeLow ? !on : on) ? HIGH : LOW);
}

// THE relay boot-flicker fix. The ORDER of these three calls is the entire
// point - it is not stylistic.
//
//   WRONG (what this file did until 2026-07-29):
//     pinMode(pin, OUTPUT);        // driver comes up with the output latch
//                                  // still at its reset value of 0 -> the pin
//                                  // is ACTIVELY DRIVEN LOW
//     digitalWrite(pin, HIGH);     // ...only now does it go to "off"
//
//   LOW is RELAY ON for these active-LOW modules, so that gap is not a
//   floating pin - it is a real, driven turn-on pulse. And it is not
//   microseconds: the ESP-IDF gpio driver logs a line from inside
//   gpio_config(), which at 115200 baud takes milliseconds. Measured in this
//   board's own boot log, consecutive relay pins were configured ~17ms apart,
//   which is comfortably long enough for a mechanical relay to actually pull
//   in and audibly click.
//
//   RIGHT (what we do now):
//     digitalWrite(pin, offLevel); // latch OFF while the pin is still high-Z.
//                                  // digitalWrite() writes GPIO.out_w1ts/w1tc
//                                  // directly regardless of pin direction, so
//                                  // this takes effect even before OUTPUT.
//     pinMode(pin, OUTPUT);        // driver comes up ALREADY holding OFF - the
//                                  // pin's first ever driven level is "off"
//     digitalWrite(pin, offLevel); // re-assert, harmless, matches the
//                                  // known-good bare sketch exactly
//
// Confirmed empirically on this exact hardware: a minimal 4-relay sketch using
// this order shows NO flicker on any of the four relays, while the old order
// flickered. That experiment also tells us the ~672ms bootloader window where
// the pins genuinely float is NOT what was causing the visible flicker - these
// relay modules tolerate a floating input fine (they bias it themselves); what
// they do not tolerate is being driven low.
static inline void relayInitOff(int pin, bool activeLow) {
  const int offLevel = activeLow ? HIGH : LOW;
  digitalWrite(pin, offLevel);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
}

void hardware_io_init() {
  relayInitOff(PIN_RELAY_LED,   RELAY_LED_ACTIVE_LOW);
  relayInitOff(PIN_RELAY_MIST,  RELAY_MIST_ACTIVE_LOW);
  relayInitOff(PIN_RELAY_PUMP,  RELAY_PUMP_ACTIVE_LOW);
  relayInitOff(PIN_RELAY_VALVE, RELAY_VALVE_ACTIVE_LOW);

  // Motor enable is active-HIGH, so LOW is its off level and the reset-default
  // latch (0) already matches - but latch-before-direction anyway, same
  // discipline as the relays, so this stays correct if the polarity ever flips.
  digitalWrite(PIN_MOTOR_EN, LOW);
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, LOW);

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

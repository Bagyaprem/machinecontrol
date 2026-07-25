#pragma once

// Every function here is the SOLE writer of its pin(s) - no other file in
// this firmware may call digitalWrite/ledcWrite on a relay or motor pin
// directly. Each device state machine calls exactly the one function below
// that owns its hardware, which is what prevents manual commands, auto
// triggers, and timers from racing each other on the same pin.

void hardware_io_init();

void hw_write_led(bool on);
void hw_write_pump(bool on);
void hw_write_mist(bool on);
void hw_write_solenoid(bool on);
void hw_write_motor_pwm(int speedPercent);   // 0/50/75/100

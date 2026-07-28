#pragma once

void solenoid_init();   // deliberately no solenoid_apply_restored_state() - hold NEVER auto-restores after reboot, see .cpp
void solenoid_trigger_pulse();      // fires the 1.02s pulse; rejected if hold is ON, ignored if already pulsing
void solenoid_set_hold(bool on);    // rejected (hold stays unchanged) if a pulse is currently running
void solenoid_update();

bool solenoid_get_hold();     // for the TFT display only
bool solenoid_get_pulsing();

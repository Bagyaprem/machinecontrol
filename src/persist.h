#pragma once

// Tiny wrapper over ESP32 NVS (Preferences) so each device state machine can
// save its last SUSTAINED command and restore it after a power cut - "the
// machine stays at the last command even through a power outage". Transient
// pulses (mist/solenoid pulse-in-flight) are deliberately NOT persisted:
// replaying a half-finished 1s pulse after a reboot has no meaning.
//
// All calls happen on Core 1 (device task) only, at human command rates -
// NVS is wear-leveled and a putBool costs a few ms, so saving on every
// accepted command is safe for both timing and flash lifetime.

void persist_init();

bool persist_get_bool(const char *key, bool def);
void persist_set_bool(const char *key, bool value);

int  persist_get_int(const char *key, int def);
void persist_set_int(const char *key, int value);

#pragma once

void pump_init();
void pump_apply_restored_state();  // called once by main.cpp's boot restore gate, not from pump_init()
void pump_set(bool on);   // called by main.cpp's command dispatcher on CMD_PUMP_SET
void pump_update();       // called every device-task loop iteration
bool pump_get_on();       // for the TFT display only - never for control decisions

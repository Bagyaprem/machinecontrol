#pragma once

void pump_init();
void pump_set(bool on);   // called by main.cpp's command dispatcher on CMD_PUMP_SET
void pump_update();       // called every device-task loop iteration
bool pump_get_on();       // for the TFT display only - never for control decisions

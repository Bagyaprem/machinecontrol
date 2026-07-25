#pragma once

void led_init();
void led_set(bool on);   // called by main.cpp's command dispatcher on CMD_LED_SET
void led_update();       // called every device-task loop iteration
bool led_get_on();       // for the TFT display only - never for control decisions

#pragma once

void mist_init();
void mist_trigger_manual(int seconds);   // 1/2/3 only; ignored if a pulse is already running (see .cpp)
void mist_set_auto(bool enabled);
void mist_update();

bool mist_get_pulsing();   // for the TFT display only
bool mist_get_auto();

#pragma once

void tft_init();
void tft_update();   // throttled internally; safe to call every device-task loop iteration

#pragma once

void schedule_init();
void schedule_set(bool enabled, int onH, int onM, int offH, int offM);
void schedule_update();   // called every device-task loop iteration

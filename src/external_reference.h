#pragma once

// Holds the backend-fetched external PM2.5 reference value for TFT DISPLAY
// ONLY. Deliberately has no bearing on any control decision - the motor's
// external-PM auto-trigger is entirely TOPIC_MOTOR_EXTERNAL_TRIGGER's
// resolved boolean (see device_motor.cpp), which arrives and is applied
// completely independently of this module.

void external_reference_set(float pm25);
bool external_reference_has_data();
float external_reference_get_pm25();

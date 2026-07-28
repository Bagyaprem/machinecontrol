#pragma once

void motor_init();
void motor_apply_restored_state();             // called once by main.cpp's boot restore gate, not from motor_init()
void motor_set_manual(int speed);              // 0/50/75/100 only - always applied regardless of auto mode ("manual check")
void motor_set_auto(bool enabled);             // master trigger switch: when true, mq135/external-PM readings can raise the speed above manual_speed
void motor_set_external_trigger(bool request); // backend-resolved request (see config.h TOPIC_MOTOR_EXTERNAL_TRIGGER) - already past the backend's own hysteresis, trusted as-is
void motor_update();                           // polls MQ135 on its own timer, runs the priority resolver, applies PWM

int motor_get_applied_speed();     // for the TFT display only
float motor_get_last_mq135_ppm();
bool motor_get_air_quality_danger();   // true if either trigger source (local MQ135 or backend-resolved external PM) is above its ON threshold right now - display only, doesn't affect the priority resolver above

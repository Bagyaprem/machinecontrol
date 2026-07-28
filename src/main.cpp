/*
 * ESP32 MQTT control firmware - dual-core FreeRTOS state machines.
 *
 *   Core 0: WiFi + MQTT only (network_task.cpp). Never touches a relay/PWM
 *           pin. Talks to Core 1 exclusively through two FreeRTOS queues.
 *   Core 1: all relay/motor/sensor/TFT logic (this file's deviceTask, plus
 *           the device_*.cpp state machines). Never makes a network call,
 *           never blocks - every timed behavior is a millis() comparison
 *           polled once per iteration.
 *
 * Devices: LED strip (relay1), air pump (relay2), mist diffuser (relay3),
 * solenoid/drain valve (relay4), motor driver (PWM), MQ135 sensor, TFT.
 * See config.h for pins/topics and each device_*.cpp for its state machine
 * and the design decisions called out in the spec (mist ignore-on-busy,
 * solenoid reject-on-conflict, motor override auto-clear).
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include "config.h"
#include "state_bus.h"
#include "hardware_io.h"
#include "device_led.h"
#include "device_pump.h"
#include "device_mist.h"
#include "device_solenoid.h"
#include "device_motor.h"
#include "device_schedule.h"
#include "external_reference.h"
#include "tft_display.h"
#include "network_task.h"
#include "persist.h"

static void dispatchCommand(const Command &cmd) {
  switch (cmd.type) {
    case CMD_LED_SET:           led_set(cmd.boolValue); break;
    case CMD_PUMP_SET:          pump_set(cmd.boolValue); break;
    case CMD_MIST_PULSE:        mist_trigger_manual(cmd.intValue); break;
    case CMD_MIST_AUTO_SET:     mist_set_auto(cmd.boolValue); break;
    case CMD_SOLENOID_PULSE:    solenoid_trigger_pulse(); break;
    case CMD_SOLENOID_HOLD_SET: solenoid_set_hold(cmd.boolValue); break;
    case CMD_MOTOR_SET:            motor_set_manual(cmd.intValue); break;
    case CMD_MOTOR_AUTO_SET:       motor_set_auto(cmd.boolValue); break;
    case CMD_EXTERNAL_TRIGGER_SET: motor_set_external_trigger(cmd.boolValue); break;
    case CMD_EXTERNAL_READING_SET: external_reference_set(cmd.floatValue); break;
    case CMD_SCHEDULE_SET:
      schedule_set(cmd.schedule.enabled, cmd.schedule.onH, cmd.schedule.onM, cmd.schedule.offH, cmd.schedule.offM);
      break;
  }
}

// Boot restore gate: hardware_io_init() already forces every relay/motor
// pin OFF as the very first thing that runs, but on a bigger firmware image
// the bootloader's own flash-load time (before setup() even starts) can
// stretch long enough for a floating GPIO to actually trip a relay coil
// before that safe-off write ever happens - see the hardware pull-up
// recommendation for that specific gap, which no firmware change can close
// since it's before any of this code runs.
//
// What THIS gate controls is different: it holds off re-applying whatever
// was PERSISTED from before a power cut (a relay that was left ON, the
// motor's last speed) until WiFi is confirmed up AND an extra 1s has
// passed - rather than restoring it within the first few milliseconds of
// boot like before. Per explicit user request: gives a longer, predictable
// "everything OFF" window right after power-on instead of the persisted
// state snapping back on almost immediately.
//
// The valve/solenoid is deliberately EXCLUDED from this restore entirely
// (not just delayed) - see device_solenoid.cpp's safety comment. Water
// flow resuming unattended after a power cut is the one case where "come
// back to how it was" is actively unsafe, so it always comes back off and
// needs a fresh manual command regardless of what it was doing before.
static bool s_bootRestoreDone = false;
static unsigned long s_wifiConnectedAtMs = 0;

static void bootRestoreGate(unsigned long now) {
  if (s_bootRestoreDone) return;
  if (!g_wifiConnected) {
    s_wifiConnectedAtMs = 0;   // reset if WiFi drops before the 1s elapses
    return;
  }
  if (s_wifiConnectedAtMs == 0) s_wifiConnectedAtMs = now;
  if (now - s_wifiConnectedAtMs < 1000UL) return;

  led_apply_restored_state();
  pump_apply_restored_state();
  // No solenoid_apply_restored_state() call - the valve deliberately never
  // auto-restores hold-on after a reboot, see device_solenoid.cpp.
  motor_apply_restored_state();
  s_bootRestoreDone = true;
}

// Core 1: all five device state machines run here, independently. Each
// update() call is a handful of millis() comparisons - none of them can
// block or wait on any other, so no device's timing can delay another's,
// and nothing here ever waits on the network.
static void deviceTask(void *pv) {
  hardware_io_init();   // all pins to safe/off first, before any restore below energizes anything
  persist_init();       // must be open before the device inits read their saved states
  led_init();
  pump_init();
  mist_init();
  solenoid_init();
  motor_init();
  schedule_init();
  tft_init();

  for (;;) {
    Command cmd;
    while (xQueueReceive(cmdQueueHandle, &cmd, 0) == pdTRUE) {
      dispatchCommand(cmd);
    }

    bootRestoreGate(millis());

    led_update();
    pump_update();
    mist_update();
    solenoid_update();
    motor_update();
    schedule_update();
    tft_update();

    // ~200 Hz. Comfortably fast enough for the 50ms timing tolerances the
    // spec asks for (mist/solenoid pulses), while still yielding to the
    // scheduler/watchdog every iteration - no busy-wait.
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Diagnostic for the "every relay drops when mist triggers" report: if that's
// actually a brownout reset (mist actuator's inrush current sags the shared
// supply rail below the ESP32's brownout threshold), every relay would go
// off because the WHOLE BOARD rebooted and hardware_io_init() re-ran - not
// because any code path writes more than one relay pin. This prints the
// reset cause so a brownout shows up unmistakably in the serial log instead
// of being guessed at. hw_write_mist() and friends are each single-pin
// writes (see hardware_io.cpp) - there is no code path from mist to any
// other relay, so if this prints ESP_RST_BROWNOUT right after a mist
// trigger, the fix is hardware (bulk capacitor + adequate/separate supply
// for the mist actuator, not a firmware change).
static void logResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *name =
    r == ESP_RST_BROWNOUT ? "BROWNOUT (supply sagged - check mist actuator's power rail)" :
    r == ESP_RST_PANIC    ? "PANIC (firmware crash)" :
    r == ESP_RST_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT ? "WATCHDOG" :
    r == ESP_RST_POWERON  ? "POWERON (normal cold boot)" :
    r == ESP_RST_SW       ? "SOFTWARE" :
    "OTHER";
  Serial.printf("[boot] reset reason: %d (%s)\n", (int)r, name);
}

void setup() {
  Serial.begin(115200);
  logResetReason();
  state_bus_init();   // queues must exist before either task can touch them

  xTaskCreatePinnedToCore(networkTaskFn, "netTask", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(deviceTask,    "devTask", 8192, nullptr, 1, nullptr, 1);
}

void loop() {
  // Everything happens in the two pinned tasks above; Arduino's default
  // loop task deletes itself immediately and is never used again.
  vTaskDelete(nullptr);
}

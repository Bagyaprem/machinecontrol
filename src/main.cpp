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

// Boot restore gate: hardware_io_init() already forces every relay/motor pin
// OFF as the very first thing that runs (before Serial, before either task -
// see setup() below). What THIS gate controls is separate: whether/when to
// re-apply whatever was PERSISTED from before a power cut (a relay that was
// left ON, the motor's last speed).
//
// Two paths to restoring, in order of preference:
//   1. WiFi confirms up, plus an extra 1s settle - the original, safer path:
//      only restore once we know the board is actually reachable/controllable
//      again, not just blindly trusting NVS.
//   2. FALLBACK (added per explicit user request): if WiFi hasn't come up
//      within BOOT_RESTORE_WIFI_TIMEOUT_MS of boot at all, restore anyway.
//      This trades away part of path 1's safety guarantee - LED/pump/motor
//      WILL come back on with zero confirmation the board can be reached or
//      commanded - in exchange for not leaving the machine stuck off
//      indefinitely if the router/AP is the thing that's slow (e.g. a whole-
//      building power cut where the router reboots slower than the ESP32).
//      Deliberately a ONE-TIME uptime deadline, not "WiFi down for 5s" - it
//      only ever fires on a cold boot where WiFi never showed up at all, not
//      on some later WiFi blip long after normal operation started.
//
// The valve/solenoid is deliberately EXCLUDED from BOTH paths, no exception -
// see device_solenoid.cpp's safety comment. Water flow resuming unattended
// after a power cut is the one case where "come back to how it was" is
// actively unsafe, so it always comes back off and needs a fresh manual
// command regardless of what it was doing before, and regardless of WiFi.
#define BOOT_RESTORE_WIFI_TIMEOUT_MS 5000UL

static bool s_bootRestoreDone = false;
static unsigned long s_wifiConnectedAtMs = 0;

static void bootRestoreGate(unsigned long now) {
  if (s_bootRestoreDone) return;

  if (g_wifiConnected) {
    if (s_wifiConnectedAtMs == 0) s_wifiConnectedAtMs = now;
    if (now - s_wifiConnectedAtMs < 1000UL) return;   // path 1: confirmed WiFi + 1s settle
  } else {
    s_wifiConnectedAtMs = 0;   // reset if WiFi drops before the 1s elapses
    if (now < BOOT_RESTORE_WIFI_TIMEOUT_MS) return;   // still within the "give WiFi a chance" window
    // else: path 2 - WiFi never came up at all within the timeout, restore anyway
  }

  led_apply_restored_state();
  pump_apply_restored_state();
  // No solenoid_apply_restored_state() call - the valve deliberately never
  // auto-restores hold-on after a reboot, on EITHER path above.
  motor_apply_restored_state();
  s_bootRestoreDone = true;
}

// Core 1: all five device state machines run here, independently. Each
// update() call is a handful of millis() comparisons - none of them can
// block or wait on any other, so no device's timing can delay another's,
// and nothing here ever waits on the network.
static void deviceTask(void *pv) {
  // hardware_io_init() is NOT called here any more - setup() now runs it as
  // its very first statement, before Serial/task creation, so the relay pins
  // reach their driven-off state as early as this firmware possibly can.
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
// Runtime self-test, not just a code-review claim: reads back the ACTUAL
// level hardware_io_init() left each pin at, and reports pass/fail per pin
// against what "off" means for that pin's own polarity (HIGH for the 4
// active-LOW relays, LOW for the active-HIGH motor-enable - see config.h).
// digitalRead() on an ESP32 OUTPUT pin reads the driver's own output
// register, not some external signal, so this genuinely confirms what the
// pin is driving, not merely what we last commanded it to. Runs on every
// single boot, so this isn't a one-time manual check - it's load-bearing:
// if a future edit ever breaks the off-state guarantee, this fires
// "UNSAFE" on the very next boot, cold or warm, with no board required to
// go re-verify by hand.
static void verifyBootSafeState() {
  struct RelayCheck { const char *name; int pin; bool activeLow; };
  static const RelayCheck relays[] = {
    {"LED",   PIN_RELAY_LED,   RELAY_LED_ACTIVE_LOW},
    {"MIST",  PIN_RELAY_MIST,  RELAY_MIST_ACTIVE_LOW},
    {"PUMP",  PIN_RELAY_PUMP,  RELAY_PUMP_ACTIVE_LOW},
    {"VALVE", PIN_RELAY_VALVE, RELAY_VALVE_ACTIVE_LOW},
  };
  bool allSafe = true;
  for (const auto &r : relays) {
    int level = digitalRead(r.pin);
    bool isOff = r.activeLow ? (level == HIGH) : (level == LOW);
    Serial.printf("[safe-boot] %-5s GPIO%-2d = %-4s -> relay %s\n",
                  r.name, r.pin, level == HIGH ? "HIGH" : "LOW",
                  isOff ? "OFF (safe)" : "*** ON - UNSAFE ***");
    allSafe = allSafe && isOff;
  }
  int enLevel = digitalRead(PIN_MOTOR_EN);
  bool enOff = (enLevel == LOW);   // motor-enable is active-HIGH, so LOW = disabled
  Serial.printf("[safe-boot] MOTOR_EN GPIO%-2d = %-4s -> driver %s\n",
                PIN_MOTOR_EN, enLevel == HIGH ? "HIGH" : "LOW",
                enOff ? "disabled (safe)" : "*** ENABLED - UNSAFE ***");
  allSafe = allSafe && enOff;

  Serial.println(allSafe
    ? "[safe-boot] ALL OUTPUTS CONFIRMED OFF"
    : "[safe-boot] *** UNSAFE STATE DETECTED AT BOOT - INVESTIGATE IMMEDIATELY ***");
}

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
  // FIRST, before Serial and before either task exists: drive every relay and
  // the motor enable to their off level. See hardware_io.cpp's relayInitOff()
  // for why the latch-then-direction order inside is what actually kills the
  // boot flicker. Running this ahead of Serial.begin()/xTaskCreate also buys
  // back the ~15ms those cost, during which the pins were previously still
  // undriven.
  hardware_io_init();

  Serial.begin(115200);
  verifyBootSafeState();   // proves, doesn't just claim, that every output reached off
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

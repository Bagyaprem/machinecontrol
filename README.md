# esp32-mqtt-control

Ground-up rebuild of the ESP32 firmware as independent, non-blocking state
machines communicating over MQTT — see `esp32-control/` for the previous
Firebase-polling version (kept untouched; this is a separate PlatformIO
project on the same physical board).

## Architecture

```
Core 0 (network_task.cpp)          Core 1 (main.cpp: deviceTask)
--------------------------          ------------------------------
WiFi + esp_mqtt_client only.        led / pump / mist / solenoid / motor
Never touches a pin.                state machines + TFT. Never makes a
                                     network call, never blocks.

        cmdQueueHandle  (Core0 -> Core1: parsed commands)
        stateQueueHandle (Core1 -> Core0: JSON state to publish)
```

Both queues are drained with a 0-tick timeout on both ends, so a full or
empty queue never stalls either core. Core 0 additionally keeps a small
cache of each retained topic's last-known JSON, so on MQTT reconnect it can
resync the broker immediately without waiting for Core 1 to notice a state
change (there may not be one — a device can sit perfectly idle through an
entire outage and still needs its retained state republished on return).

Every relay/PWM pin has exactly one function that writes it (`hardware_io.cpp`),
called only from its owning device module — see the "sole writer" comments
throughout `device_*.cpp`.

## Why the hybrid `arduino, espidf` framework

The spec requires real QoS 1 (broker acknowledgement + automatic resend) on
every command/state topic. The common Arduino MQTT library, PubSubClient,
does not do this — it only ever publishes QoS 0 regardless of the flag you
pass it. `network_task.cpp` uses ESP-IDF's `esp_mqtt_client` directly, which
does implement real QoS 1, at the cost of building with `framework = arduino,
espidf` instead of plain `arduino`. See `platformio.ini` for the resulting
first-build caveat (sdkconfig / `mqtt_client.h` not found).

**Now built and flashed successfully on real hardware** (ESP32 DevKit V1 on
COM3, PlatformIO Core 6.1.19). Getting a clean build required two fixes,
both already applied in `sdkconfig.defaults` — worth knowing about if you
ever delete/regenerate `sdkconfig.esp32doit-devkit-v1` and hit them again:

- `esp32-arduino requires CONFIG_FREERTOS_HZ=1000 (currently 100)` — the
  Arduino-as-ESP-IDF-component wrapper needs a 1000Hz tick; ESP-IDF's own
  default is 100Hz. Fixed via `CONFIG_FREERTOS_HZ=1000`.
- `undefined reference to `app_main'` at link time — nothing provides the
  ESP-IDF entry point that bridges to this project's `setup()`/`loop()`
  without `CONFIG_AUTOSTART_ARDUINO=y` explicitly set.

Confirmed on hardware: clean boot, all relay/motor/TFT GPIO pins configured
exactly as expected, MQ135 polling firing on its ~3s cadence, **no
brownout, panic, or unexpected reset** over multiple boot cycles — the
original firmware's core problem. With placeholder `WIFI_SSID`/`MQTT_HOST`
still in `config.h`, WiFi obviously can't connect and MQTT client init
fails fast on the malformed placeholder URI (confirmed non-fatal —
ESP-IDF's mqtt component degrades gracefully; `network_task.cpp` also now
explicitly guards against a NULL client handle either way). The board's
flash is a genuine 4MB (confirmed via `esptool.py flash_id`, pinned
explicitly in `sdkconfig.defaults` after the build tooling briefly assumed
2MB and warned). Flash usage is ~1.07MB of each 1.5MB OTA app partition
(68%) - see `partitions_ota.csv`.

If building on a network-mapped drive: this hit the exact same
network-drive problem as `esp32dashboard` (see that project's README) but
manifesting differently - SCons/the compiler got a mangled UNC-style path
and failed with "No such file or directory" for every source file. Building
from a local disk copy fixed it immediately; this project's working copy is
now at `C:\Users\premb\projects\esp32-mqtt-control`, not under
`Z:\claude\APPCONTROL` anymore, for the same reason the dashboard moved.

## Before you flash

This repo is **public** (needed for GitHub-pull OTA below to work with no
auth), so credentials live in `src/secrets.h` - gitignored, never committed.
On a fresh clone:

```
cp src/secrets.h.example src/secrets.h
```

then fill in `secrets.h`:
- `WIFI_SSID` / `WIFI_PASS`
- `MQTT_HOST` / `MQTT_PORT` / `MQTT_USERNAME` / `MQTT_PASSWORD`

`src/config.h` itself only has non-secret config left: `MQTT_ROOT_CA` (the
CA that signed your broker's certificate - public CA like ISRG Root X1 for
HiveMQ Cloud, or your own self-signed CA for a self-hosted Mosquitto
instance), pins, topics, and the OTA settings below.

And `platformio.ini`'s `-ota` env password, if you'll use the LAN-based
`espota` wireless upload env (separate from the GitHub-pull OTA below -
that one needs the device on the same network as your dev machine; use it
for quick iteration when the board's still reachable over WiFi locally).

Hardware pins/relay-polarity/MQ135 calibration constants were carried over
unchanged from `esp32-control`'s confirmed wiring on the same physical board.

## OTA (GitHub-pull)

For once the board's sealed up somewhere permanent and USB isn't an option.
The board polls a `manifest.json` over HTTPS and self-updates if the version
differs from what it's running - no LAN/dev-machine proximity required,
unlike the `espota` env above.

**CI/CD:** `.github/workflows/firmware-ota.yml` builds on every push to `main`
that touches `src/**`, `platformio.ini`, or the partition/sdkconfig files, then
commits the built `firmware.bin` and a matching `manifest.json` back into
`ota/` on `main` automatically - nothing to run by hand. It needs six repo
secrets (Settings > Secrets and variables > Actions) mirroring
`src/secrets.h`: `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`, `MQTT_PORT`,
`MQTT_USERNAME`, `MQTT_PASSWORD` - see `src/secrets.h.example` for what each is.

**One-time setup:** `OTA_MANIFEST_URL` in `src/config.h` already points at
this repo's `ota/manifest.json`. Flash once over USB with that baked in -
every update after this one can be OTA.

**Shipping an update after that:** bump `FIRMWARE_VERSION` in `config.h` and
push to `main` - CI does the rest. The board checks every
`OTA_CHECK_INTERVAL_MS` (6h default), on boot, and instantly if you publish
anything to `esp32/esp32monitor/ota/trigger`. Progress is reported (retained)
on `esp32/esp32monitor/ota/status`: `checking` / `up_to_date` / `updating` /
`success` / `error:<reason>`.

A failed download/flash never bricks the board - `esp_https_ota()` only
switches the boot partition over on a fully verified successful image, so
the board just keeps running its current firmware if anything goes wrong.

## Topics

| Topic | Direction | Payload | Retained | QoS |
|---|---|---|---|---|
| `esp32/esp32monitor/led/set` | subscribe | `{"on":true}` | — | 1 |
| `esp32/esp32monitor/led/state` | publish | `{"on":true}` | yes | 1 |
| `esp32/esp32monitor/pump/set` | subscribe | `{"on":true}` | — | 1 |
| `esp32/esp32monitor/pump/state` | publish | `{"on":true}` | yes | 1 |
| `esp32/esp32monitor/mist/set` | subscribe | `{"pulse_s":1\|2\|3}` or `{"auto":true\|false}` | — | 1 |
| `esp32/esp32monitor/mist/state` | publish | `{"pulsing":bool,"auto_enabled":bool,"source":"none"\|"manual"\|"auto"}` | yes | 1 |
| `esp32/esp32monitor/solenoid/set` | subscribe | `{"pulse":true}` or `{"hold":true\|false}` | — | 1 |
| `esp32/esp32monitor/solenoid/state` | publish | `{"hold":bool,"pulsing":bool}` | yes | 1 |
| `esp32/esp32monitor/solenoid/error` | publish | `{"error":"pulse_rejected_hold_active"\|"hold_on_rejected_pulse_active"}` | no | 1 |
| `esp32/esp32monitor/motor/set` | subscribe | `{"speed":0\|50\|75\|100}` | — | 1 |
| `esp32/esp32monitor/motor/state` | publish | `{"manual_speed":int,"applied_speed":int,"mq135_request":bool,"external_pm_request":bool,"override_off":bool}` | yes | 1 |
| `esp32/esp32monitor/motor/external_trigger` | subscribe | `{"request":true\|false}` — backend-resolved (Prompt 2 applies its own hysteresis to the third-party PM value before publishing this), not fetched by the ESP32 | yes | 1 |
| `esp32/esp32monitor/external_pm/reading` | subscribe | `{"co2":float,"pm25":float}` — raw values from the backend's external source (the GOGREEN device's real sensor), **TFT display only, never a control input** (see `external_reference.cpp`) | yes | 1 |
| `esp32/esp32monitor/mq135/reading` | publish | `{"raw":int,"ppm":float}` | no | 1 |
| `esp32/esp32monitor/status` | publish (LWT) | `online` / `offline` | yes | 1 |
| `esp32/esp32monitor/ota/trigger` | subscribe | any payload - forces an immediate OTA check | no | 1 |
| `esp32/esp32monitor/ota/status` | publish | `checking`\|`up_to_date`\|`updating`\|`success`\|`error:<reason>` | yes | 1 |

## Design decisions (spec asked these to be explicit)

- **Mist diffuser, duplicate trigger while pulsing:** ignored, not queued
  (`device_mist.cpp`). A dropped duplicate press is safer to reason about
  than a queued backlog firing back-to-back later.
- **Solenoid, pulse-vs-hold conflict:** rejected deterministically, reported
  on `solenoid/error` (`device_solenoid.cpp`). Hold state and pulse state
  are never both "on" and no implicit cancel-and-override step exists.
- **Motor, override_off reset:** auto-clears once both trigger sources
  (MQ135 and external PM) have dropped below their OFF thresholds — matches
  the spec's pseudocode verbatim (`device_motor.cpp`). An explicit manual
  command to a non-zero speed also clears it immediately, before that.
- **Motor, external-PM hysteresis ownership:** lives entirely in the
  Prompt 2 backend service, not on this board — the ESP32 subscribes to
  `motor/external_trigger` and trusts the already-resolved boolean as-is.
  Only the local MQ135 reading gets on-device hysteresis (`device_motor.cpp`).

## Bench-test acceptance checklist (from spec)

- [ ] Each device operates correctly in isolation via serial/manual pin
      toggling, no dashboard/database connected.
- [ ] Mist: manual 1/2/3s pulses accurate to ~50ms; auto-cycle fires every
      30 minutes for several hours without drift or missed cycles (changed
      from the original 60s spec to match the real hardware's intended
      cadence - see `device_mist.cpp`'s `MIST_AUTO_CYCLE_MS`).
- [ ] Solenoid: pulse consistently 1.02s ± 50ms; hold and pulse never
      active simultaneously.
- [ ] Motor: forcing MQ135 above/below threshold triggers/clears auto-ON
      with visible hysteresis; manual OFF override resolves per the
      documented auto-clear rule.
- [ ] Runs 24h with zero unexpected resets, including while the mist relay
      fires (validates the original brownout issue is resolved).
- [ ] All five devices can change state within the same loop iteration
      without any device's timing being delayed by another.

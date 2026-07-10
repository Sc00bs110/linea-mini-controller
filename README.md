# Linea Mini Controller

An open-source ESP32-S3 touchscreen controller for the La Marzocco Linea Mini
espresso machine.

This project replaces the Linea Mini's stock control panel with a 3.5″ colour
touchscreen, adding brew-by-weight, shot timing, a cleaning-cycle reminder,
standby scheduling, Home Assistant integration, and over-the-air firmware
updates. It talks to the machine's own GICAR control board over the existing
CN11 connector using the same serial protocol the factory panel uses — **no
modification to the machine's electronics is required**, and the stock panel
can be refitted at any time.

It is a hobby project, built from a from-scratch reimplementation of the
GICAR protocol derived from sniffing the factory panel's traffic. It is not
affiliated with, endorsed by, or supported by La Marzocco.

## Features

- **Live machine display** — boiler temperature, target temperature, shot
  timer, and machine status on a 320×480 LVGL touch UI.
- **Brew-by-weight** — pairs with a Bluetooth scale (Felicita Arc or Bookoo
  Themis Ultra) and stops the shot automatically at a target weight, with a
  configurable pre-stop allowance to account for drip.
- **Shot counting that knows the difference** — pump runs shorter than
  20 seconds are treated as group-head flushes; only runs of 20 seconds or
  more count as shots.
- **Cleaning-cycle reminder** — counts down shots to the next backflush
  (interval set in the settings menu) and flags when a clean is due.
- **Standby scheduling** — daily wake and sleep times, timezone-aware.
- **Home Assistant integration** — MQTT discovery publishes boiler
  temperature, brewing state, steam and standby switches, shot counters, a
  firmware-update button, and online/availability sensors.
- **Shot history** — per-shot temperature samples logged to flash as a
  rolling window of recent shots.
- **Self-updating firmware** — the settings menu checks this repository's
  latest GitHub release and installs it from the screen: check → confirm →
  progress → reboot. A LAN-pull path (`tools/ota_pull.py`) supports fast
  iteration during development.

## Hardware

| Part | Details |
| --- | --- |
| MCU | DFRobot FireBeetle 2 ESP32-S3 (DFR0975) — ESP32-S3-WROOM-1-N16R8, 16 MB flash, 8 MB PSRAM |
| Display | DFRobot 3.5″ GDI touch display (DFR1092), 320×480, driven as an ILI9488 panel via LovyanGFX; GT911 capacitive touch over I²C |
| Machine link | UART to the machine's CN11 connector, carrying the GICAR protocol used by the stock panel |
| Scale (optional) | Felicita Arc or Bookoo Themis Ultra, via BLE |

An ESP32-C6 build environment (FireBeetle 2 ESP32-C6 / DFR1075) is also
defined and builds from the same source, but the S3 is the actively
maintained and deployed target. Wiring notes and pin history live in
`reference/lm_mini/` and the per-environment comments in `platformio.ini`.

> **CN11 wiring caveat:** the harness's TXD/RXD labels at the CN11 plug are
> crossed relative to what the wire colours and pull-up placement suggest.
> Trust decoded frames, not the labels — this cost real debugging time twice.
> If you are rewiring, verify with a loopback test at the plug before
> assuming which wire is which.

## Building from source

Requires [PlatformIO](https://platformio.org/).

```bash
cp include/secrets.h.example include/secrets.h
# edit include/secrets.h: WiFi credentials, MQTT broker, OTA password
pio run -e lm_controller_s3
```

`include/secrets.h` is gitignored and must never be committed. The
compiled-in WiFi and MQTT values only *seed* the device's flash storage on
first boot; after that the device uses its stored copies (which is how the
CI-built release binaries — compiled with blank secrets — keep your
connectivity across updates).

A pre-build hook regenerates `include/build_info.h` (git hash + build
timestamp) on every build. The human-facing version string (`FW_VERSION`,
e.g. `v0.30`) lives in `include/version.h` and is bumped on every deployed
release.

## Firmware updates

- **From the device** — Settings → "Firmware update" checks this
  repository's latest release over HTTPS and installs it if newer than the
  running version, with an on-screen confirmation step and progress readout.
- **From your LAN** — `python tools/ota_pull.py` serves a locally-built
  `firmware.bin` over HTTP and publishes an MQTT command telling the device
  to pull and flash it. This is the fast path when iterating without cutting
  a release.

Pushing a version tag (`v*`) builds the S3 firmware in GitHub Actions and
publishes a release with `firmware.bin` and a `version.json` manifest — see
`.github/workflows/release.yml`.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Firmware: GICAR protocol driver, machine state, LVGL UI, BLE scale, MQTT/Home Assistant, OTA, shot log |
| `include/` | Headers, LVGL configuration, `version.h`, `secrets.h.example` |
| `tools/` | Build-info generator, LAN OTA server, espota fallback |
| `cad/` | OpenSCAD design and STLs for the machine-mounted display housing |
| `reference/` | Protocol captures and wiring notes |
| `.github/workflows/` | Tagged-release build pipeline |

## Safety

This project interfaces directly with a mains-powered heating appliance —
boiler, pump, and solenoids — via its OEM control board. Bugs in this
firmware could plausibly cause the machine to behave unexpectedly. Use at
your own risk. Not affiliated with, endorsed by, or supported by
La Marzocco.

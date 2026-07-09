# LineaMini Controller

A replacement touchscreen controller for the La Marzocco Linea Mini espresso
machine, built on an ESP32-S3. It replaces the stock control panel, talking
to the machine's own GICAR control board over its existing CN11 connector —
no modification to the machine's electronics.

This is a hobby project, built from a from-scratch reimplementation of a
predecessor project's GICAR protocol work. It is not affiliated with,
endorsed by, or supported by La Marzocco.

## Hardware

- **MCU**: DFRobot FireBeetle 2 ESP32-S3 (DFR0975), ESP32-S3-WROOM-1-N16R8 —
  16 MB flash, 8 MB octal PSRAM.
- **Display**: DFRobot 3.5" GDI touch display (DFR1092), 320x480, driven as
  an ILI9488 panel via LovyanGFX (the panel's own datasheet says ST7365P, but
  the ILI9488 driver renders correctly on the physical unit — see
  `platformio.ini` for the bring-up history). Capacitive touch via GT911
  (I2C) over the same GDI connector.
- **Machine interface**: UART connection to the machine's CN11 connector,
  carrying the GICAR protocol (the same protocol the stock control panel
  uses to talk to the machine's boiler/pump controller).
- An ESP32-C6 build (`env:lm_controller`, DFRobot FireBeetle 2 ESP32-C6 V1.2
  / DFR1075) is also defined in `platformio.ini` and builds from the same
  source, but the S3 is the actively maintained/deployed target.

Wiring and pin history live in `reference/lm_mini/` and in the per-env
comments in `platformio.ini` — read both before touching pin assignments.

**CN11 TXD/RXD caveat**: the wiring harness's TXD/RXD labels at the CN11 plug
are crossed relative to what the wire colors/pullup placement would suggest.
Trust decoded frames, not the labels — this cost real debugging time twice
across this project and its predecessor (see the GICAR pin comments in
`platformio.ini` for the full loopback-test writeup). If you're rewiring,
verify with a loopback test at the plug before assuming which wire is which.

## Features

- LVGL-based touch UI: live boiler temperature, shot timer, and machine
  status on a 320x480 portrait display.
- Brew-by-weight via BLE scale (Felicita Arc or Bookoo Themis Ultra) — the
  UI can stop the shot automatically at a target weight, offset by a
  pre-stop allowance to account for drip.
- Shot counters and a cleaning-cycle reminder (configurable shot interval,
  tracks shots since last clean).
- Standby scheduling (daily wake/sleep times, timezone-aware).
- Home Assistant integration via MQTT discovery — publishes boiler temp,
  target temp, brewing state, steam/standby switches, shot counters, a
  cleaning-reminder button, and machine online/availability sensors under
  `homeassistant/...`.
- Shot history log: per-shot temperature samples written to LittleFS
  (`/shots/*.json`), rolling window of the most recent shots.
- OTA updates two ways:
  - **GitHub release pull**: the UI's Settings → "Firmware update" checks
    this repo's latest release (`version.json` + `firmware.bin`) over HTTPS
    and installs it.
  - **LAN pull**: `tools/ota_pull.py` serves a locally-built `firmware.bin`
    and triggers the device to fetch and flash it over MQTT — useful for
    iterating without cutting a release.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
cp include/secrets.h.example include/secrets.h
# edit include/secrets.h: WiFi SSID/password fallback, MQTT broker, OTA password
pio run -e lm_controller_s3
```

`tools/gen_build_info.py` runs automatically as a PlatformIO pre-build hook
and regenerates `include/build_info.h` (git hash + build timestamp) on every
build — there's no manual version-bump step for that part. The human-facing
version string (`FW_VERSION`, e.g. `v0.25`) lives in `include/version.h` and
is bumped by hand on deployed releases.

`include/secrets.h` is gitignored and must never be committed. Note that
`MQTT_HOST` is only used to *seed* NVS on first boot when non-empty — once a
broker is configured (via NVS or Home Assistant), the compiled-in value is
never read again.

## OTA Updates

- **From the device**: Settings → "Firmware update" checks
  `github.com/Sc00bs110/linea-mini-controller`'s latest release and installs
  it if newer than the running `FW_VERSION`.
- **From your LAN**: `python tools/ota_pull.py [firmware.bin]` serves the
  binary over HTTP and publishes an MQTT command telling the device to pull
  and flash it — the reliable path for iterating during development.
  `tools/ota_upload.py` (espota push) is a desk-only fallback.

Tagged pushes (`v*`) build the S3 firmware and cut a GitHub Release
automatically — see `.github/workflows/release.yml`.

## Safety

This project interfaces directly with a mains-powered heating appliance
(boiler, pump, solenoids) via its OEM control board. Bugs in this firmware
could plausibly cause the machine to behave unexpectedly. Use at your own
risk. Not affiliated with or endorsed by La Marzocco.

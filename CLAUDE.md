# CLAUDE.md

This file provides guidance to Claude Code when working with code in this repository.

## Project Overview

This project ports the La Marzocco Mini controller firmware to a **DFRobot Firebeetle 2
ESP32-C6 V1.2** board with a **DFRobot 3.5" GDI display (DFR1092)**. The predecessor
project — targeting a plain ESP32-WROOM with a 4" ST7796S SPI display — lives in the
`W:\LM_Mini.git` repository; the GICAR protocol reverse-engineering, machine interface
design, and UI concepts from that project are the starting reference for this port, but
none of its firmware source is copied in directly — this is a from-scratch
reimplementation against the new board and display.

**Status:** actively porting, per the approved plan at
`C:\Users\johnb\.claude\plans\elegant-launching-quasar.md`. Porting basis: adapt the
~10 hardware-agnostic modules from `W:\LM_Mini.git` root `src/` (already a mostly-working
C6 firmware — see that plan's Context section for why this supersedes the original
"from-scratch" framing above); only the display/touch layer is a genuine rewrite.

## Hardware

- **MCU board**: DFRobot Firebeetle 2 ESP32-C6 V1.2. See
  `reference/dfrobot/firebeetle2-esp32-c6-v1.2.md` for confirmed specs/pinout.
- **Display**: DFRobot 3.5" GDI display, SKU DFR1092, connected via the board's GDI
  port. Confirmed driver IC is **ST7365P** (Sitronix) — the predecessor project's guess
  of ILI9488 (a different DFRobot SKU, DFR0669) is wrong; do not reuse any ILI9488 init
  sequence, MADCTL, or gamma table. Touch is capacitive, I2C, 5-point; exact touch IC is
  unconfirmed (GT911 is the leading candidate — verify against the physical unit before
  finalizing touch driver code). DFRobot's own `DFRobot_GDL` library
  (github.com/DFRobot/DFRobot_GDL) supports this panel directly via
  `DFRobot_ST7365P_320x480_HW_SPI`. Full details in
  `reference/dfrobot/dfr1092-3.5-display.md`.
- **GICAR UART (machine interface)**: TX/RX on GPIO16/17, carried over from the
  predecessor board's interface design. Series resistors are already in place on both
  GPIO16 (TX) and GPIO17 (RX), plus a 10 kΩ pull-up on GPIO17 (RX) — see
  `reference/lm_mini/HARDWARE_INTERFACE.md` for the electrical rationale (CN11 signals
  are 2.5 V LVCMOS; the pull-up ensures reliable logic-high detection at the ESP32's
  VIH threshold). Protocol details: `reference/lm_mini/GICAR_REGISTER_MAP.md`,
  `reference/lm_mini/gicar_protocol_analysis.md`, `reference/lm_mini/gicar-serial/`.
  GPIO16/17 are confirmed safe on the C6 (no strapping/JTAG conflict), but they are
  also the chip's default UART0 console pins — firmware must select the native
  USB-Serial-JTAG console instead of UART0 so the console doesn't fight with the GICAR
  link. See `reference/dfrobot/firebeetle2-esp32-c6-v1.2.md`.
- **SPI bus**: the C6 exposes only one usable SPI peripheral (GPIO23/22/21
  SCK/MOSI/MISO), and it's the same bus wired to the GDI connector — any other
  SPI-attached peripheral must share this bus via its own CS line.
- **No PSRAM** on this board (ESP32-C6FH4) — don't carry over any PSRAM assumption from
  the ESP32-WROOM predecessor.
- **Test/flash rig**: the board is connected to the Universal Embedded Workbench at
  `http://192.168.1.107:8080/` for serial flashing, RFC2217 remote serial, and GPIO
  control during bring-up and testing. See the `esp-pio-handling` / `esp-idf-handling`
  and `workbench-*` skills for driving it.

## Firmware Versioning

Every build must embed a fresh, auto-generated build identifier — there is no manual
version-bump step, and no build/upload path may skip regenerating it:

- `tools/gen_build_info.py` is a PlatformIO pre-build hook (wire it in with
  `extra_scripts = pre:tools/gen_build_info.py` once `platformio.ini` exists) that
  writes `include/build_info.h` on every `pio run` / `pio run -t upload`, defining
  `FW_GIT_HASH` (short git commit hash, `+dirty` suffix if the working tree has
  uncommitted changes) and `FW_BUILD_TIME` (UTC ISO-8601 build timestamp).
- `include/build_info.h` is generated, not committed — add it to `.gitignore` once
  `include/` exists as a real directory.
- Print it as a **repeating heartbeat in `loop()`** (every ~2s), not a one-shot line in
  `setup()`:
  ```cpp
  #include "build_info.h"
  void setup() { Serial.begin(115200); delay(100); }
  void loop() {
      Serial.printf("Firmware build: %s (%s)\n", FW_GIT_HASH, FW_BUILD_TIME);
      delay(2000);
  }
  ```
  This is a heartbeat, not a boot-only print, specifically so a **passive**
  `/api/serial/monitor` attach can always catch it within one period — see the
  Workbench Bench-Testing Gotchas section below for why a one-shot boot print is
  much harder to capture reliably on this board.
- This is deliberately not a semantic MAJOR.MINOR.PATCH version — it's a build
  fingerprint so any two uploads are always distinguishable over serial, with no
  reliance on a developer remembering to bump anything.

## Workbench Bench-Testing Gotchas (native-USB ESP32-C6)

Hard-won during Phase 0 bring-up — this board has no separate USB-UART bridge chip,
only the native USB-Serial-JTAG peripheral (confirmed via `usb_devices` on `/api/devices`:
`"USB JTAG/serial debug unit", vid_pid 303a:1001`). This changes normal workbench
behavior in two important ways:

1. **Once the app owns the USB-CDC console, `/api/flash`'s default auto-reset can stop
   working** — it may fail repeatedly with a pyserial "Write timeout" trying to
   re-flash a board that's actively running app firmware over native USB-CDC.
   `/api/serial/recover` and manual `/api/gpio/set` BOOT/EN toggling did **not**
   reliably fix this in testing. **What worked**: physically hold BOOT, tap RESET,
   release RESET then BOOT (classic manual bootloader entry) immediately before calling
   `/api/flash` — do this whenever a re-flash hangs on a running app.
2. **`/api/serial/reset` forces the chip into ROM `DOWNLOAD` mode on this board, not a
   normal app reset.** Its DTR/RTS toggle sequence is the classic esptool
   enter-bootloader pattern; on this native-USB C6 that pattern is interpreted as a
   genuine bootloader-entry request (confirmed via `boot:0x27 DOWNLOAD(...)` in the ROM
   banner after calling it), not a soft reset back into the app. **Do not use
   `/api/serial/reset` to capture app boot output on this board.** A debug-session-based
   JTAG reset (a JTAG "reset run" reports `Reset cause (24) - JTAG CPU reset` and does
   resume the app, unlike the DTR/RTS path) avoids forcing download mode.
3. **Even with the heartbeat print + JTAG-safe reset + passive `/api/serial/monitor`,
   no serial data came through in testing (Phases 0 and 1, repeatedly, with and without
   a preceding reset).** Root cause identified from Arduino-ESP32's actual `HWCDC.cpp`
   source (`cores/esp32/HWCDC.cpp` in `espressif/arduino-esp32`): unlike a discrete
   USB-UART bridge chip (which the ESP32-WROOM predecessor board used, and which just
   emits bytes over UART0 unconditionally regardless of whether a host is listening),
   the C6's native USB-Serial-JTAG peripheral gates TX on a `connected` flag that
   depends on detecting genuine USB Start-of-Frame traffic from an actively-connected
   host (`isCDC_Connected()`). The Arduino-ESP32 source itself documents this detection
   as flaky by Espressif's own admission (`"the SOF watchdog ... known to flap even on
   a healthy link"`, `"SOF ISR is causing esptool to be unable to upload firmware"`) —
   the same underlying flakiness likely explains the USB re-enumeration/flapping seen
   during flashing (see point 1). **This is a hardware/core-level characteristic of
   this board, not a bug in this project's firmware or a fixable workbench API call.**
   **Practical implication:** don't treat workbench serial-monitor capture as a hard
   verification requirement for this board. A real terminal (PuTTY, Arduino Serial
   Monitor) on a machine with a *direct* physical USB connection to the board — not
   relayed through the workbench's RFC2217 proxy — is more likely to see output
   reliably, since it sidesteps whatever is confusing the connection-detection here.

## Reference Material

- `reference/lm_mini/` — curated docs pulled from `W:\LM_Mini.git` (the ESP32-WROOM
  predecessor): machine/CN11 hardware interface, GICAR protocol docs, the prior C6
  bring-up wiring guide (`c6-display-wiring.html` — shows the exact GDI pin mapping used
  for the temporary ILI9341 display, chosen specifically to match DFR1092 pins so no
  rewiring would be needed on arrival), a reference-only snapshot of that project's
  `platformio.ini` (**not** an active build config for this repo), and the old UI design
  doc (targets a different, smaller display — needs adaptation, not reuse as-is).
- `reference/dfrobot/` — board and display documentation fetched from wiki.dfrobot.com.

## Repository

- Local working copy; remote is `W:\LineaMini_C6.git` (bare repo on the same Git server
  as the other project repos, e.g. `W:\LM_Mini.git`).

## Gotchas / Do Not

- Do NOT SSH into the workbench Pi to interact with the board — use the HTTP API at
  `http://192.168.1.107:8080/`.
- Do NOT assume the DFR1092's driver IC or GDI pinout — confirm against
  `reference/dfrobot/dfr1092-3.5-display.md` before writing display init code.
- Do NOT copy firmware source from `W:\LM_Mini.git` wholesale — GICAR protocol logic can
  be reused conceptually, but the display/touch layer, pin assignments, and board
  peripherals (SPI host, GPIO capabilities) differ between ESP32-WROOM and ESP32-C6.

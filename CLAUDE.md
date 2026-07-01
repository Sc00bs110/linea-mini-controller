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

**Status:** reference-gathering / pre-implementation. No firmware source exists yet in
this repo — see `reference/` for everything gathered so far in preparation for starting
the port.

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

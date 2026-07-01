# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This project replaces the existing La Marzocco Mini ESP32 daughterboard with a custom-built board that provides full machine control via Home Assistant with **no cloud dependency**. The existing daughterboard plugs into CN11 (RS-232) on the machine main board. The RS-232 protocol between the main board and the daughterboard is undocumented publicly and must be reverse-engineered. The existing board is kept as a rollback; a new board will fit the same physical location.

## Hardware

### La Marzocco Mini Interface Port
The machine exposes an RS-232 connector (CN11 on the machine PCB) with four signals:
- **GND** — ground reference
- **RXD** — machine *receives* on this pin → daughterboard must *transmit* on this line
- **TXD** — machine *transmits* on this pin → daughterboard must *receive* on this line
- **+12Vdc** — 12 V supply from the machine (powers the daughterboard)

**Empirically confirmed:** CN11 TXD/RXD signals measure 2.5 V max to GND — these are **2.5 V LVCMOS**, not true ±12 V RS-232. No level-shifter IC is required for the replacement board. The MAX3232 on the existing daughterboard is likely there for the GS3 8-pin connector (other machine models), not the Mini CN11 lines.

Replacement board interface circuit:
- 300 Ω series resistor on each signal line — **optional**, ESD/short-circuit protection only. Signals are 2.5 V LVCMOS over short wires in a closed enclosure; ESP32 GPIOs have internal ESD diodes. Omit if simplifying the build.
- 10 kΩ pull-up to 3.3 V on the ESP32 RX pin — **required**. 2.5 V is marginal for ESP32 VIH (≈ 2.47 V min); pull-up ensures reliable logic high detection.

**DIP Switch SW1 (machine main board):**
- Position 1 = OFF → simultaneous heating of both boilers (confirmed setting on this machine)
- Position 1 = ON → priority coffee boiler heating
- Position 2 = OFF → 230/240 V mains version (confirmed)

### Existing Daughterboard (ESP32 IoT Gateway)
The existing board is kept intact as a rollback — do NOT update its firmware (newer LM firmware blocks third-party API access). A replacement board will occupy the same physical location.

This is a **universal La Marzocco gateway PCB** shared across multiple machine models. Connectors for different models are co-located on the board; only the relevant one is used per machine.

Board components:
- **MCU**: Espressif ESP32-WROOM-A-H (Wi-Fi + Bluetooth)
- **Power**: LD1 3.3 V LDO regulator; switching regulator with 4.7 µH inductor; powered from machine's +12 V rail via CN11
- **Mini CN11 interface**: 4-pin connector on back of board (GND, RXD, TXD, +12Vdc) — active connector for the Linea Mini
- **GS3 interface**: 8-pin 2-row connector on back of board (adjacent to 4-pin) — NOT used on the Mini. Carries RS-232 + power + direct sensor signals: flowmeter GND, +5V ref, flowmeter pulse, tank level probe, boiler level probe. The GS3 AV gateway reads the flowmeter directly for volumetric dosing; the Mini exposes everything via RS-232 only.
- **Front 6-pin 2-row connector**: full ESP32 programming header (3.3V, GND, TX, RX, EN, GPIO0) — preferred firmware dump interface; verify by continuity test
- **RS-232 level shifter**: 8-pin SMD IC cluster near machine connectors
- **Programming pads (back of board)**: P1, P2, P3 = UART0 header (TX, RX, GND). Extra pad nearby = likely EN (reset).
- **LEDs**: LD2, LD3, LD4 (status indicators)

**Firmware dump procedure** (before any other work):
1. Connect USB-UART adapter (3.3 V logic: CP2102/CH340/FTDI) → adapter TX to board RX, adapter RX to board TX, shared GND
2. Short EN pad to GND briefly while GPIO0/BOOT is held low to enter bootloader mode
3. `esptool.py --port COM<N> --baud 921600 read_flash 0x0 0x400000 lm_gateway_backup.bin`

## Replacement Board Hardware (current target)

- **MCU**: ESP32-D0WD-V3 — use as an ESP32-WROOM-32E module (4 MB flash, no PSRAM, dual-core LX6 240 MHz, WiFi + BT 4.2). Program via USB-UART adapter (CH340/CP2102) on UART0.
- **Display**: 4" ST7796S SPI TFT, 320×480 native (480×320 in landscape). Shares VSPI bus with touch.
- **Touch**: XPT2046 resistive, shares VSPI bus with display. TOUCH_CS = GPIO33.
- **VSPI bus**: MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18
- **Display pins**: CS=GPIO15, DC=GPIO2, RST=GPIO4, BL=GPIO32
- **Touch pins**: CS=GPIO33, IRQ=GPIO27 (optional, polling supported)
- **Gicar UART1**: TX=GPIO16 (→CN11 RXD), RX=GPIO17 (←CN11 TXD, +10 kΩ pull-up to 3.3 V)
- **Button**: GPIO14 (active-LOW, INPUT_PULLUP)
- **Avoid**: GPIO0 (boot), GPIO1/3 (UART0 TX/RX), GPIO6–11 (flash SPI), GPIO12 (boot strapping)
- **Power**: CN11 +12 V → buck converter (12 V→5 V, ≥500 mA) → ESP32 VIN pin

## Firmware Architecture

- **Serial driver**: UART1 on GPIO16 TX / GPIO17 RX, invert=true. CN11 signals are 2.5 V LVCMOS — no level-shifter IC needed, just 300 Ω series + 10 kΩ pull-up on RX (GPIO17). Protocol is Gicar ASCII at 9600 8N1 (fully reverse-engineered — see GICAR_REGISTER_MAP.md).
- **Display**: TFT_eSPI configured for ST7796S SPI (VSPI bus) + LVGL. Screen 480×320 in landscape (setRotation=1). Touch via TFT_eSPI built-in XPT2046 support (TOUCH_CS=GPIO33). Touch calibration data in touch.cpp needs one-time calibration run.
- **Wi-Fi/MQTT or HTTP**: publish machine state and accept commands.
- **Home Assistant integration**: via MQTT discovery.

## Development Toolchain (anticipated)

If using ESP-IDF:
```bash
idf.py build
idf.py flash monitor
idf.py -p COM<N> flash monitor   # Windows with specific COM port
```

If using ESPHome:
```bash
esphome run <config>.yaml
esphome compile <config>.yaml
```

If using Arduino + PlatformIO:
```bash
pio run
pio run -t upload
pio device monitor
```

## Photos

Reference photos are in `Photos/`:
- `PXL_20260315_124913588.MP (1).jpg` — front of ESP32 daughterboard (shows ESP32-WROOM-A-H, P3/P2/P1 pad labels, CN3/CN4 connectors, switching regulator, LEDs)
- `PXL_20260315_124941109.MP.jpg` — back of ESP32 daughterboard (shows 4-pin Mini CN11 connector and 8-pin GS3 connector top, programming pads P1/P2/P3 bottom-right)
- `Clipboard01.png` — close-up of CN11 RS-232 pinout label (GND, RXD, TXD, +12Vdc) and DIP switch position
- `PXL_20260315_222120411.MP.jpg` — **full La Marzocco Mini wiring schematic** (shows complete machine topology, all connector labels, DIP switch definitions — primary reference document)
- `PXL_20260315_124715745.MP.jpg` — **inside the machine** showing main board (left) and daughterboard mounted in sub-enclosure (right), connected via flat ribbon cable. The ribbon cable is the CN11 harness — intercept this for logic analyser sniffing.

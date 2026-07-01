# DFRobot FireBeetle 2 ESP32-C6 (DFR1075)

Source: https://wiki.dfrobot.com/dfr1075/#tech_specs
(Landing page: https://wiki.dfrobot.com/dfr1075/ — "Tech Specs" tab)
Also cross-checked against: https://wiki.dfrobot.com/SKU_DFR1075_FireBeetle_2_Board_ESP32_C6
Product/store page: https://www.dfrobot.com/product-2771.html

Retrieved: 2026-07-01

## SKU / Version — CONFIRMED

- **SKU: DFR1075** — confirmed, matches project assumption.
- The wiki's spec table documents **three hardware revisions: V1.0, V1.1, V1.2**, distinguished mainly by the onboard power regulator and deep-sleep current (see Power section below). The silkscreen pin-diagram image on the wiki is explicitly labeled **"FireBeetle 2 ESP32-C6 V1.0"**, but the spec table itself lists V1.2 sleep-current data, so V1.2 is a real, documented revision — **the wiki does not call out any pinout differences between V1.0/V1.1/V1.2**, only power-related differences. Treat pinout as identical across revisions unless a schematic diff says otherwise.

## MCU / CPU

- Chip: **ESP32-C6FH4** (the "H4" suffix = 4 MB in-package flash, no PSRAM variant)
- Processor: **RISC-V single-core** (not Xtensa — this is a real architecture change vs. the ESP32-WROOM/Xtensa board this project is porting from)
- Clock: **160 MHz** main frequency
- SRAM: **512 KB**
- ROM: **320 KB**
- RTC SRAM: **16 KB**
- Flash: **4 MB** (in-package)
- **PSRAM: none.** Not mentioned anywhere on the wiki, and the "FH4" chip variant does not include PSRAM. Do not assume PSRAM is available — this differs from PSRAM-equipped ESP32 boards.
- USB: USB 2.0 CDC (native USB, used for programming/console — see UART note below)

## Wireless Radio

- WiFi: IEEE 802.11b/g/n **+ 802.11ax (WiFi 6, 20 MHz-only non-AP mode)**, 2.4 GHz only, 20/40 MHz bandwidth, Station/SoftAP/SoftAP+Station modes, TX/RX A-MPDU & A-MSDU aggregation
- Bluetooth: **Bluetooth 5, Bluetooth mesh**, 125 Kbps–2 Mbps
- IEEE 802.15.4 radio (2.4 GHz, 250 Kbps): supports **Thread 1.3** and **Zigbee 3.0** — board can act as Thread border router / Matter gateway / Zigbee bridge

## Peripheral Counts (per wiki "Ports" table)

- Digital I/O: **19**
- LED PWM: 6 channels
- **SPI: x1** — CONFIRMED, only one SPI peripheral is broken out on this board (see SPI section)
- UART: x3 total (includes 1 LP-UART); only one full UART (GPIO16/17) is broken out to a labeled header pin pair
- I2C: x2 (includes 1 LP-I2C)
- I2S: x1
- IR transceiver: 2 TX / 2 RX channels
- ADC: 1x 12-bit SAR ADC, 7 channels
- DMA: 3 TX / 3 RX channels

## Power

| Parameter | Value |
|---|---|
| Operating voltage | 3.3 V |
| Type-C input voltage | 5 V DC |
| VIN pin input | 5 V DC or 5 V solar panel |
| Max charging current | 0.5 A |
| Deep-sleep current (battery powered) | V1.0: 16 µA; **V1.2: 36 µA** |
| Operating temp | -10 to 60 °C |
| Dimensions | 25.4 x 60 mm (1 x 2.36") |

- Onboard regulator: **HM6245** 3.3 V low-power LDO (V1.0) or **TPS62A02DRLR** 3.3 V DC-DC converter (V1.1). The regulator used on V1.2 specifically is not called out separately on this page.
- Solar charging: **CN3165** solar power management chip (MPPT) — VIN pin doubles as solar panel input.
- **BAT** pin: direct Li-ion/Li-Po battery connector (JST-style on-board connector, not just a header pin), with onboard charge management.
- **IO0** is dual-purposed as the **battery voltage detection pin** (analog read of battery voltage) — note this if IO0/GPIO0 is needed for anything else.
- Charging LED indicator: off = not powered/fully charged, on = charging.

## Strapping / Boot / Reserved Pins

From the pin diagram, these GPIOs carry boot-strapping or JTAG functions and need care:

| GPIO | Alt. functions | Notes |
|---|---|---|
| GPIO4 | A3, LP_RX, ADC1_4, **MTMS** (JTAG) | Strapping pin on ESP32-C6 |
| GPIO5 | A4, LP_TX, ADC1_5, **MTDI** (JTAG) | Strapping pin on ESP32-C6 |
| GPIO6 | D12, LP_SDA, ADC1_6, **MTCK** (JTAG) | Strapping pin on ESP32-C6 |
| GPIO7 | D11, LP_SCL, **MTDO** (JTAG) | Strapping pin on ESP32-C6 |
| GPIO8 | D2 | Strapping pin on ESP32-C6 (general knowledge; not separately flagged on wiki page) |
| GPIO9 | D9 / **BOOT** | Labeled directly on silkscreen as the BOOT button pin — classic ESP32-C6 boot-mode strapping pin |
| GPIO15 | D13 | Onboard LED pin |
| GPIO0 | — | Repurposed as **battery voltage detection** input |

**GPIO16 and GPIO17 — SAFE for UART.** They are explicitly silkscreened and diagrammed as **"16/TX"** and **"17/RX"** — i.e., DFRobot has broken these out specifically as the board's main UART pin pair, with no JTAG, ADC, LP-peripheral, or strapping function listed for either pin. They carry no strapping role on ESP32-C6. This confirms the prior iteration's GPIO16/17 UART TX/RX reservation for the espresso-machine control-board link **carries over cleanly to this board** — no conflict found.

One caveat worth flagging for firmware bring-up (not a wiki-documented fact, but standard ESP32-C6 behavior worth verifying in code): GPIO16/17 are the chip's **default UART0** pins, which some ESP-IDF configurations use for the boot-log/console. Since this board also exposes native USB (USB-Serial-JTAG via the Type-C port) for programming and console, the console should be configured to use the native USB channel instead of UART0, freeing GPIO16/17 exclusively for the espresso-machine link. Recommend confirming `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` (or PlatformIO/Arduino equivalent) is selected instead of UART0 console during firmware setup.

## SPI Bus — CONFIRMED single SPI

The wiki's port table lists **"SPI x1"**. The only SPI signals broken out to headers are:

- GPIO23 = SCK (also silkscreened as SDIO DAT3)
- GPIO22 = MOSI (also SDIO DAT2)
- GPIO21 = MISO (also SDIO DAT1)

These same three pins are also the ones wired straight to the **GDI connector's SCLK/MOSI/MISO** (see below), so the one available SPI bus is the same bus the GDI display will use. There is no second independent SPI bus broken out on this board — the project note that "only one usable SPI peripheral" exists is **confirmed accurate**. Any other SPI-attached peripheral will need to share this bus (with its own CS line) or bit-bang a software SPI on spare GPIOs.

## Full Pinout (both header rows)

Left header (top → bottom):

| Silkscreen | GPIO | Notes |
|---|---|---|
| RST | — | Reset button |
| 3V3 | — | |
| GND | — | |
| DAT3 / SCK | GPIO23 | SPI SCK / SDIO DAT3 |
| DAT2 / MOSI | GPIO22 | SPI MOSI / SDIO DAT2 |
| DAT1 / MISO | GPIO21 | SPI MISO / SDIO DAT1 |
| DAT0 / SCL | GPIO20 | I2C SCL / SDIO DAT0 |
| CLK / SDA | GPIO19 | I2C SDA / SDIO CLK |
| D9 | GPIO9 | **BOOT strapping pin**, boot button |
| CMD / D7 | GPIO18 | SDIO CMD |
| ADC1_1 / D6 | GPIO1 | ADC1 channel 1 |
| NC | — | |
| D3 | GPIO14 | |
| D2 | GPIO8 | Strapping pin (ESP32-C6) |
| TX | GPIO16 | **UART0 TX** |
| RX | GPIO17 | **UART0 RX** |

Right header (top → bottom):

| Silkscreen | GPIO | Notes |
|---|---|---|
| VIN | — | 5 V in (USB/solar) |
| 3V3 | — | |
| GND | — | |
| SCL | GPIO20 | (mirrors left-header GPIO20) |
| SDA | GPIO19 | (mirrors left-header GPIO19) |
| A4 / LP_TX | GPIO5 | ADC1_5, LP-UART TX, **MTDI (JTAG)**, strapping |
| A3 / LP_RX | GPIO4 | ADC1_4, LP-UART RX, **MTMS (JTAG)**, strapping |
| A2 | GPIO3 | ADC1_3 |
| A1 | GPIO2 | ADC1_2 |
| NC | — | |
| D13 | GPIO15 | Onboard LED |
| D12 / LP_SDA | GPIO6 | ADC1_6, LP-I2C SDA, **MTCK (JTAG)**, strapping |
| D11 / LP_SCL | GPIO7 | LP-I2C SCL, **MTDO (JTAG)**, strapping |
| NC | — | |

(GPIO20/GPIO19 physically appear on both headers — same nets, broken out twice.)

## GDI (Gravity Display Interface) Connector — 18-pin FPC

This is the connector the DFR1092 3.5" GDI display will plug into. Pin order as documented on the wiki:

| FPC Pin # (order) | FPC Label | Board Pin / GPIO | Function |
|---|---|---|---|
| 1 | VCC | 3V3 | 3.3 V power |
| 2 | LCD_BL | GPIO15 / D13 | Backlight |
| 3 | GND | GND | Ground |
| 4 | SCLK | GPIO23 / SCK | SPI clock |
| 5 | MOSI | GPIO22 / MOSI | SPI host-out |
| 6 | MISO | GPIO21 / MISO | SPI host-in |
| 7 | LCD_DC | GPIO8 / D2 | Data/command select |
| 8 | LCD_RST | GPIO14 / D3 | Display reset |
| 9 | LCD_CS | GPIO1 / D6 | TFT chip select |
| 10 | SD_CS | GPIO18 / D7 | microSD chip select (if display has SD slot) |
| 11 | FCS | NC | Font-library chip select (not connected on this board) |
| 12 | TCS | GPIO6 / D12 | Touch controller chip select |
| 13 | SCL | GPIO20 | I2C clock (touch) |
| 14 | SDA | GPIO19 | I2C data (touch) |
| 15 | INT | GPIO7 / D1 | Touch interrupt |
| 16 | BUSY | NC | Tearing/busy signal — not connected on this board |
| 17 | X1 | NC | Custom/spare pin 1 — not connected |
| 18 | X2 | NC | Custom/spare pin 2 — not connected |

Wiki note: "When using FPC to connect the screen, please configure the corresponding pin numbers according to the GDL demo. Normally, only three pins need to be configured on different main controllers" (implies most DFRobot_GDL example code only needs CS/DC/RST configured per-board; SCLK/MOSI/MISO/power are fixed by the connector).

Displays explicitly listed as GDI-compatible on this wiki page: 1.54" 240x240 IPS, 1.8" 128x160 IPS TFT, 2.0" 320x240 IPS, 2.8" 320x240 IPS resistive touch, **3.5" 480x320 IPS capacitive touch** (this is the DFR1092 family), and a 1.51" OLED transparent display.

## Downloadable Resources (linked from wiki page, not yet fetched into this repo)

- Dimension drawing, Schematics, STP files, Datasheet — available via "Downloadable Resources" links on https://wiki.dfrobot.com/dfr1075/
- ESP32-C6FH4 chip datasheet linked from the board's "Pin & Component Description" table
- HM6245 / TPS62A02DRLR / CN3165 datasheets also linked individually from the same table

## Flags / Things That Surprised Me

1. **No PSRAM** on this chip variant (ESP32-C6FH4) — if the ported firmware assumed PSRAM was available (common on ESP32-WROOM boards with 8MB PSRAM variants), that assumption needs to be dropped or the flash-only 512KB SRAM budget needs to be checked against firmware RAM usage.
2. GPIO4/5/6/7 double as **JTAG** signals (MTMS/MTDI/MTCK/MTDO) — if this project ever wants to wire up hardware JTAG debugging, those four pins are already spoken for by other functions (LP-UART, LP-I2C, ADC) per the pinout above.
3. GPIO16/17 (UART for the espresso control board) check out clean — no strapping/JTAG/ADC conflicts — but double-check the console-output config so ESP-IDF doesn't also try to print boot logs over the same UART0 pins (native USB console is available as the alternative).
4. Single SPI bus is confirmed, and it is the same bus already wired to the GDI connector (SCLK=23/MOSI=22/MISO=21) — any additional SPI device will have to share this bus via a CS line rather than get an independent bus.

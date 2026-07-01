# LilyGo T-Display-S3 — La Marzocco Mini Interface Design

## Overview

The LilyGo T-Display-S3 mounts at the front of the machine in a 3D-printed bracket.
A 4-wire cable runs from there to a small veroboard adapter mounted near CN11 inside
the machine. The adapter provides 5V power (from CN11 +12V via a buck converter) and
level-shifted serial (Gicar ASCII, 9600 8N1).

---

## CN11 Connector (machine main board)

| Pin | Signal  | Direction         | Voltage     |
|-----|---------|-------------------|-------------|
| 1   | GND     | —                 | 0 V         |
| 2   | RXD     | Machine ← Gateway | 2.5V LVCMOS |
| 3   | TXD     | Machine → Gateway | 2.5V LVCMOS |
| 4   | +12Vdc  | Machine → Gateway | 12 V        |

> **Note:** CN11 is labelled "RS-232" but signals are empirically confirmed at
> 2.5V LVCMOS max. No level-shifter IC required.

---

## Interface Circuit

```
CN11                     Veroboard adapter              LilyGo T-Display-S3
────                     ─────────────────              ───────────────────
Pin 4 (+12V) ──────────► [XL7015 buck, adj→5V] ──────► 5V pin
Pin 1 (GND)  ──────────────────────────────────────── GND pin

                              R1 300Ω       R3 10kΩ
Pin 3 (TXD)  ─────────────────┤├──────┬─────┤├──── 3.3V
  machine transmits                   │
                                      └──────────► IO44 (UART1 RX)

                              R2 300Ω
Pin 2 (RXD)  ◄────────────────┤├───────────────── IO43 (UART1 TX)
  machine receives
```

**Firmware UART init:**
```cpp
Serial1.begin(9600, SERIAL_8N1, 44, 43, true);  // RX=IO44, TX=IO43, invert=true
```

---

## Bill of Materials

| Ref | Value | Description |
|-----|-------|-------------|
| U1  | XL7015 module | DC-DC buck, adj 5–20V out, 0.8A, 5–80V in. Set trimmer to 5.0V before connecting. |
| R1  | 300Ω ¼W | Series resistor, CN11 TXD → IO44 |
| R2  | 300Ω ¼W | Series resistor, IO43 → CN11 RXD |
| R3  | 10kΩ ¼W | Pull-up, IO44 → 3.3V |
| —   | 4-pin connector | CN11 mating connector (machine side) |
| —   | 4-core cable | CN11 adapter → veroboard: +12V, GND, TXD, RXD |
| —   | 4-core cable | Veroboard → LilyGo: 5V, GND, IO43, IO44 |

**Buck converter note:** The XL7015 module is rated 0.8A max. The LilyGo draws
≤500mA peak (ESP32-S3 WiFi active + display). Leave headroom by keeping the
display backlight below full brightness — cap it in firmware at ~80% (IO38 PWM).

---

## Component Notes

**R1, R2 — 300Ω series resistors**
- ESD and short-circuit protection on both signal lines
- At 9600 baud (104 µs/bit), RC time constant with ~10 pF GPIO capacitance ≈ 3 ns — negligible signal degradation
- Limits fault current to ≈10 mA if a line is shorted to GND or 3.3V

**R3 — 10kΩ pull-up on IO44**
- CN11 TXD idles high at 2.5V; ESP32-S3 VIH minimum ≈ 2.47V (0.75 × 3.3V)
- Pull-up raises the idle level to a solid 3.3V, giving reliable logic detection
- The machine's 2.5V driver easily overrides the pull-up when driving low (0V)
- Not needed on the TX line — IO43 outputs full 3.3V; machine input is tolerant

**Buck converter — initial setup**
1. Power the module from a bench supply at 12V (no load)
2. Measure output with a multimeter at the output pads
3. Adjust the trimmer pot until output reads exactly 5.0V
4. Re-measure under load (connect LilyGo, boot) — XL7015 output will sag slightly; re-trim if needed

---

## LilyGo T-Display-S3 Pin Connections

```
LilyGo T-Display-S3 (P1 header, bottom edge)
┌────────────────────────────────────┐
│  [1.9" ST7789V display]            │
│                                    │
│  ...  IO43  IO44  ...  5V  GND  ...│
│          │     │        │    │     │
│          │     │        │    └──── ◄── Veroboard GND
│          │     │        └───────── ◄── Buck 5V out
│          │     └────── R3(10kΩ)──► 3.3V
│          │             ◄── R1(300Ω) ◄── CN11 TXD (pin 3)
│          └──────────── R2(300Ω) ──► CN11 RXD (pin 2)
└────────────────────────────────────┘
```

IO17 and IO18 are used by the CST816S touch IC I2C bus (SCL/SDA) — do not use for serial.

---

## Physical Assembly

**Veroboard adapter** (small, ≤ 5×3 cm):
- XL7015 module soldered or mounted with header pins
- R1, R2, R3 inline between screw terminals or pin headers
- CN11 side: 4-pin header/screw terminal (+12V, GND, TXD, RXD)
- LilyGo side: 4-pin header/screw terminal (5V, GND, IO43, IO44)
- Mount near the existing daughterboard sub-enclosure

**Cable routing:**
- CN11 ribbon cable exits the main board sub-enclosure — splice or extend it
  to reach the veroboard adapter
- Run a 4-core cable from the veroboard adapter through the machine chassis to
  the front-mounted LilyGo; route away from mains/heating elements

**LilyGo front mount:**
- 3D-printed bracket at front of machine
- USB-C port accessible for firmware updates (or use OTA)
- IO14 button accessible or re-mapped to a front-panel button

---

## Verification Checklist

- [ ] Buck converter output trimmed to 5.00V at no load
- [ ] Buck converter output ≥ 4.85V with LilyGo powered and WiFi active
- [ ] LilyGo boots, display shows UI, WiFi connects
- [ ] `[gicar] frame` lines appear in serial monitor at 115200 baud
- [ ] Temperature readings visible on display (confirms Gicar RX working)
- [ ] Machine responds to a write command (confirms Gicar TX working)
- [ ] No heat on R1/R2/R3 after 30 min of operation

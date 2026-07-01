# Gicar Register Map — La Marzocco Linea Mini

Sources: logic analyser captures (PulseView .sr files), gateway firmware binary (`lm_gateway_backup.bin`),
Android APK analysis (`La Marzocco Home_2.1.3` and `3.1.6`), machine.bin AVR disassembly.

---

## Protocol Format

```
Write:    W[AAAA][LLLL][DATA][CS]
Read req: R[AAAA][LLLL][CS]
Response: Z[N][R/W][AAAA][DATA][CS]   (success)
Response: Z[N][R/W][AAAA]KO[CS]       (error)
```

- All fields are ASCII-encoded hex (uppercase)
- AAAA = 4-char register address
- LLLL = 4-char length in bytes
- CS   = 2-char checksum: `sum(all bytes from R/W char through end of DATA) mod 256`
- **Commands (gateway→machine) carry NO Z[N] prefix** — confirmed 2026-06-12 by decoding
  the D1 gateway-TX channel of the dual-channel captures: raw `R40000023DB`,
  `W00E100018258`, etc. Z[N] framing appears only on machine→gateway frames,
  where it is excluded from the checksum.
- Gateway polls `R40000023DB` every **~760 ms** (the 10 Hz 54-byte stream is
  autonomous, not 1:1 poll replies)
- Baud: 9600 8N1, 3.3V TTL, **inverted polarity** on CN11

Checksum function:
```python
def gicar_cs(payload: str) -> str:
    return f"{sum(payload.encode()) & 0xFF:02X}"
# e.g. gicar_cs("W00E10001" + "81") -> "57"
```

---

## Confirmed Registers (verified from logic analyser captures)

| Address | R/W | Len | Full Command Example       | Function                        |
|---------|-----|-----|----------------------------|---------------------------------|
| 0x4000  | R   | 35  | `R40000023DB`              | Machine state poll (main Z6 stream) |
| 0x0007  | W   | 2   | `W0007000203A2B6`          | Coffee boiler temp setpoint — 16-bit big-endian, 0.1°C units. 0x03A2=930=93.0°C. Range: 88–96°C |
| 0x00E1  | W   | 1   | `W00E100018157`            | Mode register — see values below |
| 0x0300  | R/W | 7   | `W030000070F36100516...`   | Mode config block — read then write-back after mode changes |
| 0x0000  | R   | —   | part of init sequence      | Read after mode commands (post-command init) |
| 0x0020  | R   | —   | part of init sequence      | Read after mode commands |
| 0x0310  | R   | —   | part of init sequence      | Read after mode commands |
| 0x00E2  | R   | —   | part of init sequence      | Read after mode commands |

### 0x00E1 Mode Register Values

| Write Command      | Value | Function          | CS verified |
|--------------------|-------|-------------------|-------------|
| `W00E100018157`    | 0x81  | Steam boiler ON   | ✓           |
| `W00E10001014F`    | 0x01  | Steam boiler OFF  | ✓           |
| `W00E100018258`    | 0x82  | Start cleaning cycle | ✓        |

Bit field (speculative): bit7=active command, bit1=cleaning, bit0=steam

### Post-Command Init Sequence
After writing to 0x00E1 (steam ON, cleaning): gateway reads 0x0000 → 0x0020 → 0x0310 → 0x0300 → 0x00E2,
then writes 0x0300 back unchanged. Steam OFF skips this sequence entirely.

---

## High-Confidence Registers (extracted from firmware binary)

| Address | R/W | Len | Command / Template         | Function                        |
|---------|-----|-----|----------------------------|---------------------------------|
| 0x000B  | W   | 1   | `W000B000100/101/102`      | **Pre-infusion mode** — 0x00=Disabled, 0x01=TypeA, 0x02=TypeB |
| 0x0101  | W   | 1   | `W01010010` (template)     | **Pre-infusion wet/ON time** — default 0x10. Unit: TBD (likely seconds or deciseconds) |
| 0x0111  | W   | 1   | `W01110025` (template)     | **Pre-infusion hold/OFF time** — default 0x25. Unit: TBD |
| 0x0406  | W   | 1   | `W04060001`                | **Machine standby on/off** — 0x01 = on (candidate, verify empirically) |
| 0x0009  | W   | 2   | `W00090002` (template)     | Boiler 2 temp target — multi-model gateway; likely unused on Mini |
| 0x0059  | W   | 2   | `W00590002` (template)     | Purpose unknown on Mini |
| 0x0058  | W   | 1   | `W0058000101`              | Machine reboot trigger (part of reboot sequence, not normal use) |
| 0x001F  | W   | 1   | `W001F000101`              | Part of machine reboot sequence |
| 0x8000  | W   | 1   | `W80000001`                | Brew stop — **unconfirmed**, verify empirically during active shot |

### Pre-Infusion Details
- Gateway config key names: `enable_prebrewing`, `ton_prebrewing_k1..k4`, `toff_prebrewing_k1..k4`
- App-level names: `TON_PREBREWING_K1` (wetTimeMs), `TOFF_PREBREWING_K1` (holdTimeMs)
- Three modes: Disabled / TypeA (standard pre-infusion) / TypeB
- Default timing values: 0x10=16, 0x25=37, 0x1A=26 raw units
- Registers 0x0136 (default 0x1A) and 0x0150 (default 0x25) likely cover K2–K4 groups (irrelevant on single-group Mini)
- **Action**: once board running, write 0x05 to 0x0101, observe pre-brew duration to determine unit

### Standby Details
- Gateway stores `standby_mode` and `stby_timer` in NVS (not sent to machine every boot)
- `stby_timer` = inactivity timeout (minutes) — likely handled by gateway timer, not a machine register
- 0x0406 write with 0x01 is the strongest candidate for machine wake/standby toggle

---

## Register Map (from firmware binary string table)

| Address | Read Command   | Write Command      | Notes                          |
|---------|----------------|--------------------|--------------------------------|
| 0x0000  | `R00000001`    | `W00000001`        | Machine identity/status probe  |
| 0x0006  | `R00060001`    | —                  |                                |
| 0x0007  | —              | `W00070002` + data | **Coffee temp setpoint** (CONFIRMED) |
| 0x0009  | —              | `W00090002` + data | Boiler 2 temp (multi-model)    |
| 0x000B  | —              | `W000B0001` + val  | **Pre-infusion mode** (0/1/2)  |
| 0x001F  | —              | `W001F000101`      | Reboot sequence                |
| 0x0020  | `R0020002C`    | —                  | 44-byte read                   |
| 0x0050  | `R00500018`    | —                  | 24-byte read                   |
| 0x0058  | —              | `W0058000101`      | Machine reboot trigger         |
| 0x0059  | —              | `W00590002` + data | Unknown on Mini                |
| 0x00E1  | —              | `W00E10001` + val  | **Mode register** (CONFIRMED)  |
| 0x00E2  | `R00E20001`    | —                  | Post-command init read         |
| 0x0100  | `R01000001`    | `W010000015A` / `W0100000100` | |
| 0x0101  | —              | `W01010001` + val  | **Pre-infusion wet/ON time**   |
| 0x0111  | —              | `W01110001` + val  | **Pre-infusion hold/OFF time** |
| 0x0136  | —              | `W01360001` + val  | Pre-infusion timing K2 (unused on Mini) |
| 0x0150  | —              | `W01500001` + val  | Pre-infusion timing K2 (unused on Mini) |
| 0x0300  | —              | `W03000007`        | Mode config block write-back   |
| 0x0310  | `R0310001D`    | `W03100001`        | 29-byte read                   |
| 0x0400  | —              | `W04000003` / `W04000007` | Boot/mode control         |
| 0x0406  | —              | `W04060001`        | **Standby on/off** (candidate) |
| 0x0410  | `R0410001D`    | `W04100001`        | 29-byte read                   |
| 0x4000  | `R40000023DB`  | —                  | **Machine state poll** (CONFIRMED) — 35-byte Z6 stream frame |
| 0x4001  | `R40010001`    | —                  | Realtime data                  |
| 0x4002  | `R40020001`    | —                  | Realtime data                  |
| 0x4003  | `R4003000C`    | —                  | 12-byte realtime data          |
| 0x4010  | `R40100001`    | —                  | Realtime data                  |
| 0x8000  | —              | `W80000001`        | Brew stop (unconfirmed)        |
| 0x8002–0x8007 | —       | `W800x0001`        | Advanced group/boiler control  |

---

## Machine Stream Frame (54 bytes, ~10 Hz, autonomous)

The machine broadcasts 54-byte frames at ~10 Hz regardless of polling.

| Byte offset | Content                          | Notes                              |
|-------------|----------------------------------|------------------------------------|
| 0           | 'Z' (0x5A)                       | Frame start                        |
| 1           | 0x36 = 54                        | Total frame length (NOT a seq echo — gateway TX has no seq) |
| 2–5         | '0000' (ASCII)                   | Register address 0x0000            |
| 28–37       | State bytes                      | **All non-zero even at IDLE** (`9A AA 9A 8A 82 8A 82 82 82 82` in idle + cleaning captures). Idle-vs-brewing diff NOT yet decoded. |
| 38–39       | Frame type indicator             | 0x9A09 = temperature frame (else rolling counter) |
| 40–47       | Coffee boiler temp (ASCII hex)   | `int(frame[40:48], 16) / 10.0` °C |
| 48–51       | Steam temp (ASCII hex)           | Always '0000' — pressurestat only  |

Brew detection: **payload[17] = 0x01 while brewing, 0x00 at idle** — confirmed from live
shot capture (2026-06-14). Lever pull sets the bit; shot completion clears it.
Temperature drops ~5°C during a 36s shot (94→89°C) as expected.

---

## Machine Identity

- Type: LINEA (code 3), model tag `NEWMINIL` in AVR firmware
- Serial: Sn2011010551
- Firmware: v2.12, hardware v0
- Groups: 1, Boilers: 2 (1 coffee, 1 steam)
- Steam boiler: pressurestat-controlled — **no temperature register, no software setpoint**
- Flow meter: FALSE (volumetric dosing not available on this machine)
- Main board MCU: ATmega32 @ 8 MHz, 9600 baud UART

---

## Verified Command Checksums

| Command String        | Checksum calc                          | Result |
|-----------------------|----------------------------------------|--------|
| `W00E10001` + `81`    | sum(W,0,0,E,1,0,0,0,1,8,1) mod 256    | 0x57 ✓ |
| `W00E10001` + `01`    | sum(W,0,0,E,1,0,0,0,1,0,1) mod 256    | 0x4F ✓ |
| `W00E10001` + `82`    | sum(W,0,0,E,1,0,0,0,1,8,2) mod 256    | 0x58 ✓ |
| `W0007000203A2`       | sum(W,0,0,0,7,0,0,0,2,0,3,A,2) mod 256| 0xB6 ✓ |
| `R40000023`           | sum(R,4,0,0,0,0,0,2,3) mod 256        | 0xDB ✓ |

---

## Pending Verification (empirical tests once board is running)

1. **Pre-infusion timing unit**: Write 0x05 to 0x0101, observe wet-time duration
2. **Standby command**: Write 0x01/0x00 to 0x0406, observe machine response
3. **Brew stop**: Send `W80000001` + CS during active shot, observe pump stop
4. **Pre-infusion mode K2 registers**: 0x0136 and 0x0150 — likely irrelevant on single-group Mini

---

*Last updated: 2026-05-23*
*Sources: firmware binary analysis (lm_gateway_backup.bin), PulseView logic analyser captures, La Marzocco Home APK v2.1.3 + v3.1.6*

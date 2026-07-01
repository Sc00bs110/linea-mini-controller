# La Marzocco Mini Controller — UI Design

Display: LilyGo T-Display-S3, ST7789V 320×170 landscape (rotation=3, USB-left)  
Framework: LVGL 8.4 — dark theme, Montserrat fonts  
Input: CST816S capacitive touch + IO14 button (both active simultaneously)

---

## Color Palette

| Role            | Hex       | Usage                              |
|-----------------|-----------|------------------------------------|
| Background      | `#000000` | All screens                        |
| Amber primary   | `#D4891A` | Temperature, key values            |
| Amber bright    | `#F0A830` | Edit-mode values                   |
| Green           | `#5CB85C` | Brew active, SHOT label            |
| Orange          | `#FF7043` | Steam active pill                  |
| White           | `#FFFFFF` | Shot timer text, selected items    |
| Grey mid        | `#888888` | Status, secondary labels           |
| Grey dim        | `#555555` | Shots counter, hint text           |
| Grey very dim   | `#2A2A2A` | Hint overlays, invisible-until-lit |
| Blue highlight  | `#2878D8` | Selected settings row (20% opacity)|

---

## Typography

| Font              | Size | Usage                                   |
|-------------------|------|-----------------------------------------|
| Montserrat 48     | 48px | Temperature (main), elapsed time (timer)|
| Montserrat 32     | 32px | (reserved for future use)               |
| Montserrat 24     | 24px | (reserved)                              |
| Montserrat 20     | 20px | BREWING label, SHOT label, timer temp   |
| Montserrat 16     | 16px | Status, shots, settings items, hints    |

---

## Screen 1 — Main

Default screen. Shows machine state at a glance.

```
┌────────────────────────────────────────────────────────────────┐ 320px
│                            ┌──────────────┐                    │
│                            │    STEAM     │  ← orange if on    │ 8px
│  93.5°C                    └──────────────┘                    │
│                                                                │ 85px (mid)
│  (amber M48)                   BREWING  ← green M20, hidden   │
│                                                                │
│                                0 shots  ← grey M16            │
│─────────────────────────────────────────────────────────────── │
│  192.168.1.113  |  Demo mode            ← grey M16            │ 148px
│  short=demo brew  long=menu  tap=settings  ← very dim         │ 164px
└────────────────────────────────────────────────────────────────┘ 170px
```

**Widgets:**
- `lbl_temp` — LEFT_MID (8, -12), M48, amber `#D4891A`
- `obj_steam` — TOP_RIGHT (-8, 8), 100×26, radius=13, bg dark → orange when active
  - `lbl_steam` — centered M16 inside pill
- `lbl_brew` — RIGHT_MID (-8, 8), M20, green, hidden when not brewing
- `lbl_shots` — RIGHT_MID (-8, 40), M16, dim grey
- `lbl_status` — BOTTOM_LEFT (8, -24), M16, grey — IP + machine state
- `lbl_hint` — BOTTOM_LEFT (8, -6), M16, very dim grey — controls hint

**Controls:**
| Input            | Action                                    |
|------------------|-------------------------------------------|
| Touch (any area) | Open Settings screen                      |
| IO14 short press | Toggle 15s demo brew (when not connected) |
| IO14 long press  | Open Settings screen                      |

**Auto-transitions:**
- brew active → auto-switch to Timer screen (shot count incremented)

---

## Screen 2 — Shot Timer

Shown automatically when a brew starts. Returns to Main 3s after brew ends,
or immediately on tap.

```
┌────────────────────────────────────────────────────────────────┐
│                                                                │
│  28.5 s                          93.5°C   ← amber M20         │
│  (white M48)                                                   │
│                                  SHOT     ← green M20         │
│                                                                │
│                                         Shot #3  ← dim grey   │
└────────────────────────────────────────────────────────────────┘
```

**Widgets:**
- `lbl_shot_time` — LEFT_MID (8, -12), M48, white — `"28.5 s"` format
- `lbl_timer_temp` — RIGHT_MID (-8, -12), M20, amber — live temp
- `lbl_shot_label` — RIGHT_MID (-8, 16), M20, green, static text "SHOT"
- `lbl_timer_shots` — BOTTOM_RIGHT (-8, -6), M16, dim grey — "Shot #N"

**Controls:**
| Input                  | Action                                    |
|------------------------|-------------------------------------------|
| Touch (any area)       | Abort brew, return to Main immediately    |
| IO14 (any press)       | No action                                 |

**Auto-transitions:**
- brew ends → stays on timer for 3s then auto-returns to Main

---

## Screen 3 — Settings

Entered from Main via long-press or tap. All 6 items fit within 170px.

```
┌────────────────────────────────────────────────────────────────┐ ← 26px header
│  SETTINGS       btn:short=next  long=select  tap=cycle  (dim) │
├────────────────────────────────────────────────────────────────┤
│ > Coffee temp                              93.5°C  ← selected │ 24px
│   Pre-infusion                               OFF              │ 24px
│   Steam                                      OFF              │ 24px
│   Auto-standby                               OFF              │ 24px
│   Clean cycle                                Run              │ 24px
│   Back                                                        │ 24px
└────────────────────────────────────────────────────────────────┘
```

Header: `#181818` bg, `lbl_settings_hdr` M20 grey left, hint M16 very dim right.

Each row (320×24): highlight = blue (#2878D8) at 20% opacity on selected row.
- Name label: LEFT_MID (4, 0), M16, grey → white when selected
- Value label: RIGHT_MID (-8, 0), M16, dim → amber when selected → `< val >` in bright amber when editing

**Settings items:**

| # | Name          | Values                      | Step    | Command sent on confirm      |
|---|---------------|-----------------------------|---------|------------------------------|
| 0 | Coffee temp   | 88.0–96.0°C                 | 0.5°C   | `machine_set_temp()`         |
| 1 | Pre-infusion  | OFF, 0.5–10.0 s             | 0.5s    | (TODO — register TBD)        |
| 2 | Steam         | ON / OFF                    | toggle  | `machine_set_steam()`        |
| 3 | Auto-standby  | OFF, 15, 30, 60 min         | cycle   | (TODO — task #16)            |
| 4 | Schedule      | — (navigate)                | —       | opens Schedule List screen   |
| 5 | Clean cycle   | — (action)                  | —       | opens Cleaning Cycle flow    |
| 6 | Back          | — (action)                  | —       | `settings_save()` + main     |

**Button controls:**
| Input             | Action                                          |
|-------------------|-------------------------------------------------|
| Short press       | Move `>` cursor to next item (wraps)            |
| Long press (item) | Enter edit mode — value shows `< val >`         |
| Short press (edit)| Cycle value one step                            |
| Long press (edit) | Confirm + save + exit edit mode                 |
| Long press (Back) | Save + return to Main                           |

**Touch controls:**
| Input             | Action                                          |
|-------------------|-------------------------------------------------|
| Tap any value row | Immediately cycle value one step (no edit mode) |
| Tap Back row      | Save + return to Main                           |
| Tap Clean row     | Trigger clean cycle                             |

Both controls are live simultaneously — button and touch can be mixed freely.

---

## Navigation Flow

```
                  ┌─────────┐
      ┌──touch────►         ◄──────brew ends (3s)──┐
      │  long btn ► MAIN    │                       │
      │           │         ├────brew starts───────►TIMER
      │           └────┬────┘                       │  │
      │                │ touch / long btn            │  touch
      │                ▼                             │  (abort)
      │           SETTINGS                          │  │
      │           │       │                         │  │
      └───────────┘       └─────────────────────────┘  │
       tap Back /                                       │
       long btn on Back                                 │
                                         ◄─────────────┘
```

---

## Cleaning Cycle Flow (multi-screen guided procedure)

Entered from Settings → "Clean cycle". Seven steps, cannot be skipped.

### Step 1 — Confirmation
```
┌────────────────────────────────────────────────────────────────┐
│  CLEANING CYCLE                                                │
│                                                                │
│  Takes approx. 2 minutes.                                      │
│  Insert blind basket with cleaning powder                      │
│  and lock portafilter in place.                               │
│                                                                │
│          [Cancel]              [Start Cleaning]                │
└────────────────────────────────────────────────────────────────┘
```

### Step 2 — Paddle Prompt (cleaning)
After "Start Cleaning" is tapped: send Gicar `W00E1000182` (cleaning command).
```
┌────────────────────────────────────────────────────────────────┐
│  CLEANING CYCLE                                                │
│                                                                │
│  Move paddle to start cleaning                                 │
│                                                                │
│                    15                                          │
│              (countdown seconds)                               │
│                                                                │
│  ────────────────────────── (progress bar)                     │
└────────────────────────────────────────────────────────────────┘
```
- Monitor Gicar state bytes [28..37] for brew-active (paddle moved)
- Paddle detected → proceed to Step 3
- 15s elapsed, no paddle → show "Cleaning cancelled" + return to settings

### Step 3 — Cleaning in Progress
```
┌────────────────────────────────────────────────────────────────┐
│  CLEANING CYCLE                                                │
│                                                                │
│  Cleaning in progress...                                       │
│                                                                │
│  Cycle  3 / 10      ████████░░░░░░░░░░░░  30s                 │
│  (3 seconds ON, 3 seconds OFF per cycle)                       │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```
- 10 cycles × (3s ON + 3s OFF) = ~60 seconds total
- Show cycle counter if detectable from Gicar state; otherwise show elapsed timer
- State bytes return to normal → proceed to Step 4

### Step 4 — Rinse Preparation
```
┌────────────────────────────────────────────────────────────────┐
│  CLEANING CYCLE                                                │
│                                                                │
│  Cleaning complete.                                            │
│                                                                │
│  Remove portafilter, rinse it thoroughly,                      │
│  then lock it back in place.                                   │
│                                                                │
│          [Cancel]              [Start Rinse]                   │
└────────────────────────────────────────────────────────────────┘
```

### Step 5 — Paddle Prompt (rinse)
After "Start Rinse" tapped: send `W00E1000182` again.
Same countdown and paddle-detection logic as Step 2.

### Step 6 — Rinse in Progress
Same display as Step 3, text reads "Rinsing...".  
10 cycles × (3s ON + 3s OFF).

### Step 7 — Complete
```
┌────────────────────────────────────────────────────────────────┐
│                                                                │
│                   Cleaning Complete                            │
│                                                                │
│              30 May 2026  14:23                                │
│                                                                │
│              (returns to settings in 3s)                       │
│                                                                │
└────────────────────────────────────────────────────────────────┘
```
- Record Unix timestamp to NVS: namespace `"maintenance"`, key `"last_clean_ts"`
- Display formatted date/time
- Auto-return to settings after 3 seconds

### Gicar Command
- `W00E1000182` (register 0x00E1, value 0x82, checksum 58) — confirmed via logic analyser
- Same command used for both cleaning and rinse cycles
- Machine requires user paddle movement to actually start the pump cycle

### NVS
- Namespace: `"maintenance"`, key: `"last_clean_ts"` (uint32, Unix epoch)
- Exposed in Home Assistant as sensor `last_cleaning` (ISO datetime string)

---

## Future Screens (planned)

### Screen 4 — Shot Detail (task #21)
Shows after a brew completes. Displays: shot time, yield (grams, from BLE scale),
ratio (dose:yield), temperature profile. Tap to dismiss.

### Screen 5 — Schedule (task #16)
Weekly on/off schedule editor. Grid of days × time slots.
Navigation: swipe left/right for days, tap to toggle time slots.

### Screen 6 — Scale / Brew-by-Weight Timer (tasks #19, #20, #21)

Scale: **Felicita Arc** (BLE). Replaces plain shot timer when scale is connected.

```
┌────────────────────────────────────────────────────────────────┐
│  28.5 s          ████████████░░░░░  36.2 → 40g   93.5°C       │
│  (M32, white)    (weight progress bar, green)     (M20, amber) │
│                                                                │
│  Shot #3                           1 : 2.3  (dose:yield ratio)│
└────────────────────────────────────────────────────────────────┘
```

**Auto-stop:** when live weight reaches `(target - offset)`, send brew-stop
command (`W80000001` — verify empirically, task #17). The offset compensates
for liquid still dripping after pump stops.

**Settings additions** (in settings menu):
- Brew target weight: 20–60g, step 1g (default 36g)
- Pre-stop offset: 0–8g, step 0.5g (default 3g) — stop at `target - offset`
- Scale: connect / disconnect / show status

---

## BLE Scale Integration — Architecture & Plan (tasks #19, #20, #21, #22)

**Scales on order:** Felicita Arc + Bookoo Themis Ultra  
Both must be supported simultaneously. Auto-detect on BLE scan — connect to whichever
is found first (or whichever is in range). No user configuration required.

---

### Firmware Architecture — Abstract Scale Driver

Both scales are wrapped behind a common interface so brew-by-weight, the shot
timer screen, and HA reporting never need to know which scale is connected:

```cpp
// src/scale.h

enum ScaleModel { SCALE_NONE, SCALE_FELICITA_ARC, SCALE_BOOKOO_THEMIS };

struct ScaleState {
    ScaleModel model;
    bool       connected;
    float      weight_g;   // live reading, grams
    bool       stable;     // weight stable (not changing rapidly)
};

extern ScaleState scale;

void  scale_init();                // start BLE scan task
void  scale_tare();                // send tare command to active scale
float scale_weight();              // current weight in grams (0.0 if disconnected)
bool  scale_connected();
const char* scale_name();          // "Felicita Arc" / "Bookoo Themis Ultra" / "—"
```

Internal: BLE task on core 0 scans for both device name prefixes, connects to the
first found, dispatches notifications to the correct parser, populates `scale`.
On disconnect, resumes scanning.

---

### BLE Protocol — Felicita Arc

| Item | Value |
|------|-------|
| Device name prefix | `"FELICITA"` |
| Service UUID | `0xFFE0` |
| Notify characteristic | `0xFFE1` |
| Packet format | 10 bytes: `[cmd][weight_hi][weight_lo][sign][unit][...]` |
| Weight | `(byte[1] << 8 \| byte[2]) / 10.0` g; negative if `byte[3] == 1` |
| Update rate | ~10 Hz |

> Verify UUIDs and packet layout with **nRF Connect** before coding — Arc format
> differs slightly from older Felicita models.

---

### BLE Protocol — Bookoo Themis Ultra

| Item | Value |
|------|-------|
| Device name prefix | `"Bookoo"` or `"BOOKOO"` |
| Service UUID | **TBD — scan with nRF Connect** |
| Notify characteristic | **TBD** |
| Packet format | **TBD** |
| Update rate | ~10 Hz (typical for espresso scales) |

> The Bookoo Themis Ultra is a newer device and its BLE protocol is less documented
> publicly. Scan with nRF Connect to capture the service/characteristic UUIDs and
> record a few notification payloads during weighing. This is needed before the
> Bookoo driver can be written.

---

### Brew-by-Weight Auto-Stop

```
During active brew, every 100 ms:
  w = scale_weight()
  if (scale_connected() && w >= (brew_target_g - prestop_offset_g)):
      gicar_write(0x8000, &stop_val, 1)   // brew stop — verify task #17
      record yield = w + drip_estimate
      mark shot as weight-stopped
      stop checking until next brew
```

**Pre-stop offset** (lag compensation):
- Espresso drips 2–4s after pump stops, adding 3–5g
- Default: 3g offset — stop pump at `target − 3g`, yield reaches ~target
- User-adjustable 0–8g in settings

---

### Shot Timer Screen — With Scale Active

```
┌────────────────────────────────────────────────────────────────┐
│  28.5 s                                            93.5°C      │
│                                                                │
│          36.2 g  ████████████████░░░░  / 40 g                 │
│          (M48 green)      (progress bar)                       │
│                                                                │
│  Shot #3                                    ratio  1 : 2.4    │
└────────────────────────────────────────────────────────────────┘
```

Without scale connected — falls back to plain elapsed timer (existing behaviour).

### Main Screen — Scale Status (right panel, below shot counter)
- Scale connected: `"36.2 g"` dim green M16
- Scanning: `"Scale..."` very dim grey M16
- Offline after timeout: nothing shown (no clutter)

---

### Settings Additions

| Item | Values | Default |
|------|--------|---------|
| Brew weight | 20–60 g, step 1 g | 36 g |
| Pre-stop offset | 0.0–8.0 g, step 0.5 g | 3.0 g |
| Scale status | display-only: name + RSSI or "Scanning..." | — |

---

### Home Assistant Entities (task #22)

| Entity | Type | Notes |
|--------|------|-------|
| `sensor.lm_scale_weight` | g | Live reading during shot |
| `sensor.lm_last_yield` | g | Yield of last completed shot |
| `sensor.lm_last_ratio` | — | Dose:yield (requires dose input) |
| `number.lm_brew_target` | g | Target brew weight |
| `number.lm_prestop_offset` | g | Pre-stop offset |
| `binary_sensor.lm_scale_connected` | — | BLE connection state |
| `sensor.lm_scale_model` | string | "Felicita Arc" / "Bookoo Themis Ultra" |

---

### Adaptive Pre-Stop Offset Algorithm

**Background — existing La Marzocco firmware:**
The gateway binary contains a `GicarAcaiaScaleMachine` class confirming scale-based
brew-stop. The NVS keys found (`scale-nm0`, `scale-nm1`, `scale-ack`) are scale
identification only — no adaptive offset keys (`scale-offset`, `scale-last-yield`,
`scale-learning-rate` etc.) were found. It is therefore **unknown** whether the
original firmware had adaptive learning; the evidence leans against it but is not
conclusive.

**Our implementation plan — adaptive pre-stop offset:**

After each completed shot where a scale was connected, record the final yield weight
(scale reading 5–10 seconds after pump stops, once dripping has slowed) and adjust
the stored offset using an exponential moving average:

```
error      = actual_yield_g - brew_target_g   // + means overshot, − means undershot
new_offset = old_offset + (error × 0.4)       // learning rate 0.4 → converges in ~4 shots
```

- Learning rate 0.4: corrects 40% of the error per shot — converges in 3–4 shots
  without overshooting
- Clamp `new_offset` to [0.0, 8.0] — safety bounds, same as manual setting
- Only update when `|error| > 0.5g` — ignore noise within half a gram
- Store in NVS (namespace `"scale"`, key `"offset"`) — survives power cycles
- Per-target tuning: offset at 36g target differs from 40g; optionally store as
  `"off_36"`, `"off_40"`, etc. (round to nearest gram) — implement if simple offset
  proves insufficiently accurate across different target weights

**Implementation order:**
1. Implement fixed offset first (done)
2. Add final-yield measurement once real scale arrives (read weight ~8s after brew ends)
3. Add NVS offset storage as part of task #15
4. Add adaptive update logic — ~20 lines, straightforward once NVS is in place

**Verification:** pull 5 shots with a consistent dose, record actual yield each time,
confirm the offset converges. If it oscillates, reduce learning rate to 0.25.

---

### Open Questions Before Coding Scale Drivers

1. **Bookoo Themis Ultra BLE:** scan with nRF Connect, record service UUID, char UUID, and 3–4 notification payloads at known weights (e.g. 10g, 20g, 30g) to reverse the encoding.
2. **Felicita Arc packet format:** confirm byte layout matches above — Arc may differ from Felicita Parallel/Brew models.
3. **Tare behaviour:** does each scale auto-tare on stable zero, or does firmware need to send an explicit tare command? Check nRF Connect for write characteristics.
4. **Brew-stop command:** verify `W80000001` empirically (task #17) before enabling auto-stop.
5. **Simultaneous scan:** confirm ESP32-S3 BLE stack handles scanning while a connection is already active (needed if user has both scales on the bench).

---

### Home Assistant integration (task #14)
No dedicated screen. HA controls are reflected in the settings screen
(same values, synced via MQTT). A small "HA" icon in the status line
indicates cloud sync active.

---

## Touch Coordinate Mapping

CST816S native: portrait X∈[0,169], Y∈[0,319]  
Display rotation=3 (landscape, USB-left) mapping:

```
display_x = SCREEN_W - 1 - raw_y   (319 - raw_y)
display_y = raw_x
```

If tap position appears mirrored, swap to Option B in `touch.cpp`:
```
display_x = raw_y
display_y = SCREEN_H - 1 - raw_x
```

---

## Pin Summary

| GPIO | Function            | Notes                              |
|------|---------------------|------------------------------------|
| 43   | Gicar UART1 TX      | CN11 → machine RXD, via 300Ω      |
| 44   | Gicar UART1 RX      | CN11 → machine TXD, 300Ω+10kΩ PU |
| 17   | Touch I2C SCL       | CST816S on T-Display-S3            |
| 18   | Touch I2C SDA       | CST816S on T-Display-S3            |
| 21   | Touch RST           | Active-low reset                   |
| 16   | Touch INT           | Interrupt (polled, not used)       |
| 14   | IO14 button         | Active-low, BUTTON_2               |
| 15   | LCD power enable    | Must be HIGH before TFT init       |
| 38   | Backlight           | TFT_BL, driven HIGH                |

Display parallel bus: D0-D7 = IO39-48, WR=IO8, RD=IO9, DC=IO7, CS=IO6, RST=IO5

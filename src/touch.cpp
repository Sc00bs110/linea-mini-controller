// Phase 4 -- GT911 capacitive touch -> LVGL pointer indev.
//
// Why this file exists: ui.cpp registers LVGL LV_EVENT_CLICKED callbacks
// (main_click_cb, timer_click_cb, settings_row_click_cb, wifi_* ...), but LVGL
// only synthesizes CLICKED from a registered LV_INDEV_TYPE_POINTER device. No
// indev existed anywhere in the codebase, so every touch-driven screen action was
// dead. This registers a GT911-backed pointer indev so those callbacks fire.
//
// Minimal direct-over-Wire GT911 protocol (no external library), consistent with
// this project's style of bit-banging small register protocols itself (see
// gicar.cpp) rather than pulling in a heavy dependency. The GT911 register map is
// well documented and tiny for single-touch use:
//   0x8140  product id  ("911\0")            -- used for the address probe
//   0x814E  status: bit7 = buffer ready, low nibble = touch-point count
//   0x8150  point 0: [track_id, x_lo, x_hi, y_lo, y_hi, size_lo, size_hi, resv]
// After each read the status byte MUST be written back to 0 or the chip stops
// refreshing the point buffer.
//
// GDI touch pins on the Firebeetle 2 ESP32-C6 + DFR1092 (confirmed): the TOUCH_*
// macros live in platformio.ini build_flags (single source of truth, same
// convention as the TFT_* display pins).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "touch.h"
#include "wlog.h"

// ── GT911 register addresses (16-bit, big-endian on the wire) ───────────────
static const uint16_t GT911_REG_PRODUCT_ID = 0x8140;  // 4 bytes, ASCII "911\0"
static const uint16_t GT911_REG_STATUS     = 0x814E;  // status / touch count
// Point-1 block starts at 0x814F: [track_id, x_lo, x_hi, y_lo, y_hi, ...].
// (Was 0x8150, which is already x_lo — parsing that as track_id shifted every
// byte by one, making both coordinates read as garbage that clamped to the
// bottom-right corner pixel 479,319 on every tap. Found via the WiFi-screen
// tap logger, 2026-07-03.)
static const uint16_t GT911_REG_POINT0     = 0x814F;  // first touch-point block

static const uint8_t GT911_STATUS_READY = 0x80;  // bit7: coordinate buffer ready
static const uint8_t GT911_COUNT_MASK   = 0x0F;  // low nibble: number of points

// GT911's 7-bit I2C address is latched from the INT pin level during its reset
// release: INT low -> 0x5D, INT high -> 0x14. We drive the reset selecting 0x5D,
// but still probe both because this exact DFR1092 SKU was never factory-confirmed
// as GT911 (see the Phase 4 plan) and some modules ship strapped for 0x14.
static const uint8_t GT911_ADDR_5D = 0x5D;
static const uint8_t GT911_ADDR_14 = 0x14;

// LVGL logical resolution after display.cpp's setRotation(5): 480 wide x 320 tall
// landscape. Kept in sync with display.cpp's SCREEN_W/SCREEN_H.
static const int16_t LV_HOR = 480;
static const int16_t LV_VER = 320;

// GT911 native panel geometry (portrait), matching the ILI9488's rotation-0 frame
// and the TFT_WIDTH/TFT_HEIGHT the panel is configured with.
static const int16_t PANEL_W = 320;  // native x span -> raw x in [0, 319]
static const int16_t PANEL_H = 480;  // native y span -> raw y in [0, 479]

// Resolved once in touch_init(); 0 means "no GT911 responded" (indev stays
// registered but always reports released, which is harmless).
static uint8_t g_gt911_addr = 0;

// ── Coordinate transform: GT911 native frame -> LVGL logical (480x320) ──────
//
// This is the ONE load-bearing, hardware-unverifiable piece, so it lives in a
// single small function with the alternative candidates written out for the
// four-corner bring-up check.
//
// Derivation: display.cpp draws via LovyanGFX setRotation(5). LovyanGFX's own
// touch path, Panel_Device::convertRawXY(), is *by construction* the exact
// inverse of that draw transform. For _internal_rotation == 5 (offset_rotation 0,
// identity affine) its branches evaluate to:
//     r & 1 (5&1=1) -> swap(x, y)        => tx = raw_y, ty = raw_x
//     r & 2 (5&2=0) -> no x flip
//     vflip: (1<<5) not in 0b10010110    => no y flip
// i.e. a pure transpose: lvgl_x = raw_y, lvgl_y = raw_x.
//
// (display.cpp describes rotation 5 as "rotation 1 + mirror-X". That is
// consistent, not contradictory: because rotation 5 swaps the axes, a mirror on
// the *native X* edge surfaces as the vflip/ty term in logical space. No need to
// re-litigate the prose -- the r==5 bit math above is authoritative and matches
// the display 1:1.)
//
// IMPORTANT CAVEAT (why a human verifies corners): the transpose above only nails
// the LCD-native-frame -> logical step. It assumes the GT911's raw output frame is
// mounted 1:1 with the ILI9488 rotation-0 frame (raw x across the 320 edge, raw y
// across the 480 edge, same origin). That glue-between-two-parts fact cannot be
// proven from source -- it is exactly what the four-corner touch test validates.
// If a corner comes out swapped or inverted, the fix is a one-line edit here:
//   * axes swapped  : use  lvgl_x = raw_x;            lvgl_y = raw_y;
//   * X inverted    : use  lvgl_x = (PANEL_H-1) - raw_y;
//   * Y inverted    : use  lvgl_y = (PANEL_W-1) - raw_x;
static void gt911_to_lvgl(int16_t raw_x, int16_t raw_y, int16_t* out_x, int16_t* out_y) {
    int16_t lx = raw_y;  // native Y edge (480) -> logical X (480)
    int16_t ly = raw_x;  // native X edge (320) -> logical Y (320)

    // Clamp to the logical panel so a stray out-of-range sample can never index
    // outside LVGL's coordinate space.
    if (lx < 0) lx = 0; else if (lx > LV_HOR - 1) lx = LV_HOR - 1;
    if (ly < 0) ly = 0; else if (ly > LV_VER - 1) ly = LV_VER - 1;

    *out_x = lx;
    *out_y = ly;
}

// ── Low-level GT911 register I/O over Wire ──────────────────────────────────
// GT911 uses a 16-bit register pointer sent big-endian, then a repeated-start
// read. Returns true only if the addressed device ACKed and delivered all bytes.
static bool gt911_read(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) {  // repeated start, keep the bus
        return false;
    }
    const size_t got = Wire.requestFrom((int)addr, (int)len);
    if (got != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return true;
}

static bool gt911_write8(uint8_t addr, uint16_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission(true) == 0;
}

// Hardware reset selecting I2C address 0x5D: hold INT low across the reset
// release, per the Goodix GT911 power-on timing. We poll (never use INT for
// interrupts), so INT is left as a plain input afterwards.
static void gt911_reset_select_5d() {
    pinMode(TOUCH_RST, OUTPUT);
    pinMode(TOUCH_INT, OUTPUT);

    digitalWrite(TOUCH_INT, LOW);
    digitalWrite(TOUCH_RST, LOW);
    delay(11);                       // hold reset asserted

    digitalWrite(TOUCH_INT, LOW);    // INT low at release -> address 0x5D
    delayMicroseconds(110);
    digitalWrite(TOUCH_RST, HIGH);   // release reset
    delay(6);                        // keep INT held while the address latches

    digitalWrite(TOUCH_INT, LOW);
    delay(51);                       // GT911 firmware settle before I2C

    pinMode(TOUCH_INT, INPUT);       // release INT (polling mode; no ISR)
    delay(50);
}

// Try one address by reading the product-id register; log the ASCII id on success.
static bool gt911_probe(uint8_t addr) {
    uint8_t id[4] = {0, 0, 0, 0};
    if (!gt911_read(addr, GT911_REG_PRODUCT_ID, id, sizeof(id))) {
        return false;
    }
    wlogf("[touch] GT911 found at 0x%02X (id=%c%c%c%c)\n",
          addr, id[0] ? id[0] : '?', id[1] ? id[1] : '?',
          id[2] ? id[2] : '?', id[3] ? id[3] : '?');
    return true;
}

// ── LVGL read callback ──────────────────────────────────────────────────────
// LVGL convention: on release report LV_INDEV_STATE_REL but keep the last x/y so
// the click lands on the widget the finger lifted from. We only ever need a
// single touch point (this UI is taps only -- no gestures/multi-touch).
static void touch_read_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;

    data->point.x = last_x;
    data->point.y = last_y;
    data->state   = LV_INDEV_STATE_REL;

    if (g_gt911_addr == 0) {
        return;  // no controller responded -- always released
    }

    uint8_t status = 0;
    if (!gt911_read(g_gt911_addr, GT911_REG_STATUS, &status, 1)) {
        return;  // transient bus error -- treat as released, retry next tick
    }

    // Only trust the point buffer once the chip flags it ready (bit7).
    if (status & GT911_STATUS_READY) {
        const uint8_t count = status & GT911_COUNT_MASK;
        if (count > 0) {
            uint8_t pt[5];  // track_id, x_lo, x_hi, y_lo, y_hi
            if (gt911_read(g_gt911_addr, GT911_REG_POINT0, pt, sizeof(pt))) {
                const int16_t raw_x = (int16_t)((uint16_t)pt[1] | ((uint16_t)pt[2] << 8));
                const int16_t raw_y = (int16_t)((uint16_t)pt[3] | ((uint16_t)pt[4] << 8));
                gt911_to_lvgl(raw_x, raw_y, &last_x, &last_y);
                data->point.x = last_x;
                data->point.y = last_y;
                data->state   = LV_INDEV_STATE_PR;
            }
        }
        // Must clear the status byte or the GT911 stops refreshing the buffer.
        gt911_write8(g_gt911_addr, GT911_REG_STATUS, 0x00);
    }
}

void touch_init() {
    // 400 kHz is well within GT911's rating and keeps the polled read cheap.
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(400000);

    // Deterministic power-on selecting 0x5D, then probe 0x5D first and 0x14 as a
    // fallback (see GT911_ADDR_* note above).
    gt911_reset_select_5d();

    if (gt911_probe(GT911_ADDR_5D)) {
        g_gt911_addr = GT911_ADDR_5D;
    } else if (gt911_probe(GT911_ADDR_14)) {
        g_gt911_addr = GT911_ADDR_14;
    } else {
        g_gt911_addr = 0;
        wlogf("[touch] no GT911 at 0x%02X or 0x%02X -- touch disabled\n",
              GT911_ADDR_5D, GT911_ADDR_14);
    }

    // Register the pointer indev regardless: if no controller answered it simply
    // reports "always released", which is harmless and keeps the wiring uniform.
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
}

// Phase 3 -- display + LVGL bring-up.
//
// Drives the DFRobot 3.5" GDI display (SKU DFR1092) through LovyanGFX, treating
// it as an ILI9488 panel, and registers it as LVGL's display driver. The
// DFR1092's datasheet nominally lists a ST7365P controller, but the
// DFRobot_GDL/ST7365P path never lit the physical panel; LovyanGFX's ILI9488
// driver was confirmed to render correctly on the real hardware, so that is what
// we use here. The panel is native 320x480 portrait; setRotation(5) presents it
// as the 480x320 landscape the espresso UI expects, with X mirrored to correct
// the DFR1092 GDI physical orientation.
//
// GDI-to-GPIO mapping (from platformio.ini build_flags, single source of truth;
// the actual pin numbers are per-env -- C6 vs S3 differ, see platformio.ini):
//   SCLK = TFT_SCLK   MOSI = TFT_MOSI   MISO = TFT_MISO
//   LCD_DC = TFT_DC   LCD_RST = TFT_RST   LCD_CS = TFT_CS
//   LCD_BL = TFT_BL (PWM backlight)
// Panel geometry comes from TFT_WIDTH (320) / TFT_HEIGHT (480).

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "display.h"

// ── LovyanGFX device definition ─────────────────────────────────────────────
// Configuration below is the empirically-confirmed-working combination for the
// DFR1092 on this board: SPI2_HOST (a user SPI peripheral valid on both the C6
// and the S3), mode 0, a 20 MHz write clock (the ILI9488's practical maximum),
// and a PWM backlight on TFT_BL. Pins come from the TFT_* macros so pin
// assignments live only in platformio.ini (per-env). Do not "tidy" the
// odd-looking Light_PWM values (freq 44100, channel 7) -- this exact block is
// what lit the physical panel.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;   // user SPI peripheral (valid on C6 and S3)
            cfg.spi_mode   = 0;
            cfg.freq_write = 20000000;    // ILI9488 max SPI write is ~20MHz
            cfg.freq_read  = 16000000;
            cfg.pin_sclk   = TFT_SCLK;    // per-env, see platformio.ini
            cfg.pin_mosi   = TFT_MOSI;    // per-env, see platformio.ini
            cfg.pin_miso   = TFT_MISO;    // per-env, see platformio.ini
            cfg.pin_dc     = TFT_DC;      // per-env, see platformio.ini
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = TFT_CS;    // per-env, see platformio.ini
            cfg.pin_rst      = TFT_RST;   // per-env, see platformio.ini
            cfg.pin_busy     = -1;
            cfg.panel_width  = TFT_WIDTH;  // 320 (portrait native)
            cfg.panel_height = TFT_HEIGHT; // 480 (portrait native)
            // This DFR1092 panel variant reports every color as its exact
            // inverse without this (confirmed on real hardware: blue<->yellow,
            // white<->black, red<->cyan, etc.) -- a panel-manufacturing-variant
            // difference from the "reference" ILI9488, not a bug in the pattern
            // or bus config (position/shape were already correct).
            cfg.invert       = true;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = TFT_BL;     // per-env, see platformio.ini
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

// File-local so it cannot ODR-collide with any global `tft` main.cpp may define.
static LGFX tft;

// Landscape + mirror-X orientation (rotation 5) -> 480 wide x 320 tall, matching
// the UI layout. Rotation 5 (not 1) corrects the DFR1092 GDI physical orientation
// -- this was determined empirically on the real panel during Phase 2.
static const uint8_t DISPLAY_ROTATION = 5;

// LVGL logical resolution after setRotation(5): 480 wide x 320 tall landscape.
static const uint32_t SCREEN_W = 480;
static const uint32_t SCREEN_H = 320;

// ── LVGL display driver ─────────────────────────────────────────────────────
// Single partial draw buffer covering 10 full rows (480 * 10 = 4800 px). This is
// the exact buffer size used by the confirmed-working reference bring-up. LVGL
// renders a dirty region into this buffer, then disp_flush() blits it to the
// panel; a partial buffer in internal RAM keeps memory low and avoids relying on
// PSRAM (the C6 has none; the S3 does, but this path does not need it).
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         lv_buf[SCREEN_W * 10];

// Flush a rendered region to the panel. This is the confirmed-working LovyanGFX
// integration from the reference bring-up: setAddrWindow + writePixels over a
// single startWrite/endWrite transaction, with color_p reinterpreted as
// LovyanGFX's rgb565_t (matches LV_COLOR_DEPTH 16 in lv_conf.h).
static void disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((lgfx::rgb565_t*)color_p, w * h);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void display_init() {
    // LovyanGFX owns the SPI bus (Bus_SPI) and the backlight pin (Light_PWM via
    // LEDC); do NOT touch SPI.begin() or drive GPIO15 manually -- that would
    // fight the driver's LEDC control of the backlight.
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(255);

    // Bring up LVGL and register this panel as its display driver. Order matters:
    // ui_init() (called next from setup()) requires lv_disp_drv_register() to have
    // already run -- see ui.h.
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, lv_buf, nullptr, SCREEN_W * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;   // 480 (landscape, per setRotation(5))
    disp_drv.ver_res  = SCREEN_H;   // 320
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}

// Phase 2 -- raw display bring-up (no LVGL).
//
// Drives the DFRobot 3.5" GDI display (SKU DFR1092) through LovyanGFX, treating
// it as an ILI9488 panel. The DFR1092's datasheet nominally lists a ST7365P
// controller, but the DFRobot_GDL/ST7365P path never lit the physical panel;
// LovyanGFX's ILI9488 driver was confirmed to render correctly on the real
// hardware, so that is what we use here. The panel is native 320x480 portrait;
// setRotation(5) presents it as the 480x320 landscape the espresso UI expects in
// later phases, with X mirrored to correct the DFR1092 GDI physical orientation.
//
// GDI-to-GPIO mapping (from platformio.ini build_flags, single source of truth):
//   SCLK = TFT_SCLK (23)  MOSI = TFT_MOSI (22)  MISO = TFT_MISO (21)
//   LCD_DC = TFT_DC (8)   LCD_RST = TFT_RST (14)  LCD_CS = TFT_CS (1)
//   LCD_BL = TFT_BL (15, PWM backlight)
// Panel geometry comes from TFT_WIDTH (320) / TFT_HEIGHT (480).

#include <Arduino.h>
#include <LovyanGFX.hpp>

#include "display.h"

// ── LovyanGFX device definition ─────────────────────────────────────────────
// Configuration below is the empirically-confirmed-working combination for the
// DFR1092 on this board: SPI2_HOST (the C6's only user SPI peripheral), mode 0,
// a 20 MHz write clock (the ILI9488's practical maximum), and a PWM backlight on
// GPIO15. Pins come from the TFT_* macros so pin assignments live only in
// platformio.ini. Do not "tidy" the odd-looking Light_PWM values (freq 44100,
// channel 7) -- this exact block is what lit the physical panel.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;   // ESP32-C6: only user SPI peripheral
            cfg.spi_mode   = 0;
            cfg.freq_write = 20000000;    // ILI9488 max SPI write is ~20MHz
            cfg.freq_read  = 16000000;
            cfg.pin_sclk   = TFT_SCLK;    // 23
            cfg.pin_mosi   = TFT_MOSI;    // 22
            cfg.pin_miso   = TFT_MISO;    // 21
            cfg.pin_dc     = TFT_DC;      // 8
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = TFT_CS;    // 1
            cfg.pin_rst      = TFT_RST;   // 14
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
            cfg.pin_bl      = TFT_BL;     // 15
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
// the UI layout ported in Phase 3. Kept as a named constant so the Phase 2
// exit-gate (visual orientation check) has a single place to adjust if it reads
// mirrored. Rotation 5 (not 1) corrects the DFR1092 GDI physical orientation --
// this was determined empirically on the real panel.
static const uint8_t DISPLAY_ROTATION = 5;

// Paint an intentionally asymmetric pattern so a human can spot a mirror or
// 180-degree flip at a glance: a distinct color in each corner (all different),
// a single origin marker box anchored at the top-left, and a white frame. A
// symmetric border alone could not reveal orientation errors.
static void draw_test_pattern() {
    const int16_t w = tft.width();
    const int16_t h = tft.height();

    // Solid, distinctive background so a lit-but-dead-controller panel (which
    // would show noise/white) is obviously distinguishable from a working one.
    tft.fillScreen(TFT_BLUE);

    // White frame: 4 px inset border on all four edges.
    const int16_t b = 4;
    tft.fillRect(0, 0, w, b, TFT_WHITE);          // top
    tft.fillRect(0, h - b, w, b, TFT_WHITE);      // bottom
    tft.fillRect(0, 0, b, h, TFT_WHITE);          // left
    tft.fillRect(w - b, 0, b, h, TFT_WHITE);      // right

    // Corner swatches -- every corner a different color so mirror/flip shows up.
    const int16_t c = 48;  // swatch size
    tft.fillRect(0, 0, c, c, TFT_RED);            // top-left
    tft.fillRect(w - c, 0, c, c, TFT_GREEN);      // top-right
    tft.fillRect(0, h - c, c, c, TFT_YELLOW);     // bottom-left
    tft.fillRect(w - c, h - c, c, c, TFT_MAGENTA);// bottom-right

    // Origin marker: an orange box just inside the top-left, unique in the
    // frame, so "which corner is (0,0)" is unambiguous when read on the panel.
    tft.fillRect(c + 8, 8, 40, 24, TFT_ORANGE);

    // Center block: a white rectangle to confirm mid-panel pixels render.
    const int16_t cw = w / 3;
    const int16_t ch = h / 3;
    tft.fillRect((w - cw) / 2, (h - ch) / 2, cw, ch, TFT_WHITE);
}

void display_init_and_test_pattern() {
    // LovyanGFX owns the SPI bus (Bus_SPI) and the backlight pin (Light_PWM via
    // LEDC); do NOT touch SPI.begin() or drive GPIO15 manually -- that would
    // fight the driver's LEDC control of the backlight.
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.setBrightness(255);

    draw_test_pattern();
}

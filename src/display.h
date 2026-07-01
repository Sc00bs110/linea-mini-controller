#pragma once

// Phase 2 -- raw display bring-up for the DFRobot 3.5" GDI panel (DFR1092) on
// the Firebeetle 2 ESP32-C6's GDI connector.
//
// This is a hardware smoke test only: it initializes the panel, turns on the
// backlight, and paints a solid fill plus an asymmetric colored test pattern so
// a human can confirm the panel lights up with the right colors and is not
// mirrored/rotated. LVGL and the real UI are wired in during Phase 3, which is
// why this lives in its own translation unit rather than in main.cpp.
//
// The panel is driven through LovyanGFX's ILI9488 driver, not DFRobot_GDL/
// ST7365P. An earlier bring-up session confirmed the ILI9488 command set renders
// correctly on the physical DFR1092 (the ST7365P attempt left the panel dark).

// Initialize the panel over hardware SPI, enable the backlight, and draw the
// bring-up test pattern. Call once from setup() after gicar_init().
void display_init_and_test_pattern();

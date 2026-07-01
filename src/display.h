#pragma once

// Phase 3 -- display + LVGL bring-up for the DFRobot 3.5" GDI panel (DFR1092) on
// the Firebeetle 2 ESP32-C6's GDI connector.
//
// The panel is driven through LovyanGFX's ILI9488 driver (not DFRobot_GDL/
// ST7365P -- an earlier bring-up session confirmed the ILI9488 command set
// renders correctly on the physical DFR1092, whereas the ST7365P attempt left
// the panel dark). This translation unit owns the LovyanGFX device and wires it
// into LVGL as the display driver, so the rest of the firmware only ever talks to
// LVGL, never to the panel directly.

// Initialize the panel over hardware SPI, enable the backlight, run lv_init(),
// and register the LVGL display driver (flush callback + draw buffer). Call once
// from setup() BEFORE ui_init() (ui.h requires lv_disp_drv_register() to have
// completed). After this returns, drive rendering with lv_timer_handler().
void display_init();

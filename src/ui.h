#pragma once

// Call after lv_init() + lv_disp_drv_register() are complete.
void ui_init();

// Call every loop() iteration — handles button events, screen transitions,
// and periodic label updates.
void ui_tick();

// Call from ArduinoOTA.onStart() callback to show OTA progress on screen.
void ui_ota_start();

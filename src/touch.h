#pragma once

// GT911 I2C capacitive touch -> LVGL pointer input device.
//
// Follows the button.h / display.h style: plain C-style free functions, no
// classes. touch_init() owns everything internally (Wire bring-up, GT911 reset +
// address probe, and lv_indev_drv registration), exactly the way display.cpp owns
// its lv_disp_drv_t. main.cpp just needs a single touch_init() call in setup().
void touch_init();   // call once from setup(), AFTER ui_init()/the first
                     // lv_timer_handler() -- see the comment above touch_init()
                     // in touch.cpp for why (S3 I2C wedge if called earlier).

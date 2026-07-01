#pragma once

void button_init();
void button_update();        // call once per loop()
bool button_short_press();   // BTN1: true for one tick on short release (<600 ms)
bool button_long_press();    // BTN1: true for one tick when held 600 ms (fires before release)
bool button2_short_press();  // BTN2: true for one tick on short release (<600 ms)
bool button2_long_press();   // BTN2: true for one tick when held 600 ms (fires before release)

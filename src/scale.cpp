// Phase 3 stub -- real BLE/WiFi/MQTT logic ported in Phase 6.
//
// The real scale.cpp drives a NimBLE central that connects to a Felicita Arc /
// Bookoo Themis scale. That pulls in NimBLE-Arduino and a BLE scan task, which
// are explicitly out of scope for the "does the LVGL UI render" phase. This stub
// satisfies scale.h's API (so ui.cpp compiles and links unchanged) while
// reporting a permanently-disconnected scale.

#include "scale.h"

// Real definition of the extern instance ui.cpp reads. Zero-initialised = no
// scale, weight 0, disconnected.
ScaleState scale = {};

void scale_init() {
    // No BLE bring-up in Phase 3.
}

void scale_tare() {
    // No scale connected -- nothing to tare.
}

void scale_tare_and_start() {
    // No scale connected -- nothing to tare/start.
}

bool scale_connected() {
    return false;
}

float scale_weight() {
    return 0.0f;
}

const char* scale_model_name() {
    return "\xE2\x80\x94";  // "—" (em dash), matching the real driver's "no scale" label
}

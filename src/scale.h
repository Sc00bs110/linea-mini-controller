#pragma once
#include <stdint.h>
#include <stdbool.h>

enum ScaleModel { SCALE_NONE, SCALE_FELICITA_ARC, SCALE_BOOKOO_THEMIS };

struct ScaleState {
    ScaleModel    model;
    bool          connected;
    volatile float    weight_g;     // grams — updated from BLE notification (core 0)
    volatile float    flow_gps;     // g/s — Bookoo only
    volatile uint32_t timer_ms;     // scale timer — Bookoo only
    volatile uint8_t  battery_pct;  // 0–100 — Bookoo only
    bool          stable;
};

extern ScaleState scale;

// Call once after lv_init, before WiFi — starts BLE scan task on core 0
void  scale_init();

// Send tare command to active scale
void  scale_tare();

// Bookoo only: tare weight and start timer atomically (use at shot start)
void  scale_tare_and_start();

bool        scale_connected();
float       scale_weight();         // current weight in grams (0.0 if disconnected)
const char* scale_model_name();     // "Felicita Arc" / "Bookoo Themis Ultra" / "—"

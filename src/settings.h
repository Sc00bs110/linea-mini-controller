#pragma once
#include <stdint.h>
#include <stdbool.h>

struct Settings {
    float    coffee_temp_c;       // 88.0–96.0, step 0.5
    float    preinfusion_s;       // 0.0–10.0, step 0.5 (0 = disabled)
    bool     steam_on;
    int      standby_min;         // 0=off, 15, 30, 60
    float    brew_target_g;       // 20–60 g, step 1 g
    float    prestop_offset_g;    // 0–8 g, step 0.5 g
    uint32_t shot_count;          // total shots pulled, persisted to NVS
    uint32_t last_cleaning_epoch; // Unix timestamp of last cleaning (0 = never)

    // Standby schedule (v0.21): absolute-time daily sleep/wake, minutes since
    // local midnight. Distinct from standby_min (the idle timeout).
    bool     sched_enabled;
    uint16_t sched_wake_min;      // e.g. 390 = 06:30
    uint16_t sched_sleep_min;     // e.g. 1200 = 20:00
    int16_t  tz_offset_min;       // local time = UTC + this; settings-menu item
};

extern Settings settings;

void settings_init();  // load from NVS (or defaults on first boot)
void settings_save();  // persist all fields to NVS

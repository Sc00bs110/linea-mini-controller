#include "button.h"
#include <Arduino.h>

#define BTN_PIN      10    // BTN1 — GPIO10/D10, active-LOW (INPUT_PULLUP)
#define BTN2_PIN      2    // BTN2 — GPIO2/D0,  active-LOW (INPUT_PULLUP)
#define DEBOUNCE_MS  30
#define LONG_MS     600

// ─── Button 1 state ───────────────────────────────────────────────────────────
static bool     b1_last_raw       = HIGH;
static bool     b1_db_state       = HIGH;
static uint32_t b1_db_time        = 0;
static uint32_t b1_press_start    = 0;
static bool     b1_long_triggered = false;
static bool     b1_short_fired    = false;
static bool     b1_long_fired     = false;

// ─── Button 2 state ───────────────────────────────────────────────────────────
static bool     b2_last_raw       = HIGH;
static bool     b2_db_state       = HIGH;
static uint32_t b2_db_time        = 0;
static uint32_t b2_press_start    = 0;
static bool     b2_long_triggered = false;
static bool     b2_short_fired    = false;
static bool     b2_long_fired     = false;

void button_init() {
    pinMode(BTN_PIN,  INPUT_PULLUP);
    pinMode(BTN2_PIN, INPUT_PULLUP);
}

static void update_one(int pin,
                       bool& last_raw, bool& db_state, uint32_t& db_time,
                       uint32_t& press_start, bool& long_triggered,
                       bool& short_fired, bool& long_fired) {
    short_fired = false;
    long_fired  = false;

    bool raw = (bool)digitalRead(pin);
    uint32_t now = millis();

    if (raw != last_raw) { last_raw = raw; db_time = now; }
    if ((now - db_time) < DEBOUNCE_MS) return;

    bool stable = raw;
    if (stable != db_state) {
        db_state = stable;
        if (db_state == LOW) {
            press_start    = now;
            long_triggered = false;
        } else {
            if (!long_triggered) short_fired = true;
        }
    }

    // Long press fires immediately at threshold, before release
    if (db_state == LOW && !long_triggered && (now - press_start) >= LONG_MS) {
        long_fired     = true;
        long_triggered = true;
    }
}

void button_update() {
    update_one(BTN_PIN,  b1_last_raw, b1_db_state, b1_db_time,
               b1_press_start, b1_long_triggered, b1_short_fired, b1_long_fired);
    update_one(BTN2_PIN, b2_last_raw, b2_db_state, b2_db_time,
               b2_press_start, b2_long_triggered, b2_short_fired, b2_long_fired);
}

bool button_short_press()  { return b1_short_fired; }
bool button_long_press()   { return b1_long_fired;  }
bool button2_short_press() { return b2_short_fired; }
bool button2_long_press()  { return b2_long_fired;  }

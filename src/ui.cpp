#include "ui.h"
#include "button.h"
#include "settings.h"
#include "machine.h"
#include "scale.h"
#include "wifi_config.h"
#include "wifi_ap.h"
#include "gicar.h"
#include "mqtt.h"
#include "wlog.h"
#include <time.h>

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

extern uint32_t wifi_retry_count();

// Custom Montserrat BOLD subset (space, digits, '.', '-', 'C', 'g', 's',
// degree) for the big temp and shot-time readouts. Generated 2026-07-03:
// fontTools.varLib.instancer wght=700, then npx lv_font_conv --size 72
// --bpp 4 --range 0x20,0x2D,0x2E,0x30-0x39,0x43,0x67,0x73,0xB0.
// Defined in src/fonts/lv_font_lm72_bold.c.
LV_FONT_DECLARE(lv_font_lm72_bold);

#define FW_VERSION "v0.21"

// ─── Forward declarations ──────────────────────────────────────────────────────
static void ui_show_main();
static void ui_show_timer();
static void ui_show_settings();
static void ui_show_sched();
static void ui_show_wifi();
static void ui_show_wifi_ap();
static void settings_draw();
static void settings_edit_increment();
static void menu_long_press_cb(lv_event_t *e);
static void sleep_wake_cb(lv_event_t *e);
static void timer_click_cb(lv_event_t *e);
static void settings_row_click_cb(lv_event_t *e);

// ─── Settings items ────────────────────────────────────────────────────────────
enum SettingsItem {
    SI_BREW_WEIGHT,  // 0  20–60 g, step 1 g
    SI_PREINFUSION,  // 1  OFF / 0.5–10.0 s, step 0.5
    SI_STANDBY,      // 2  OFF / 15 / 30 / 60 min
    SI_SCHEDULE,     // 3  navigate → standby schedule screen
    SI_TIMEZONE,     // 4  UTC offset, ±30 min steps
    SI_PRESTOP,      // 5  0.0–8.0 g, step 0.5 g
    SI_STEAM,        // 6  ON / OFF
    SI_WIFI,         // 7  navigate → WiFi status screen
    SI_BACK,         // 8  action — save + return to main
    SI_COUNT         // 9
};
// Clean cycle moved to a hold-to-start button on the Main screen (v0.16);
// coffee temp moved to the main-screen edge adjuster (v0.20).

static const char *item_names[SI_COUNT] = {
    "Brew weight",
    "Pre-infusion",
    "Auto-standby",
    "Schedule",
    "Timezone",
    "Pre-stop offset",
    "Steam",
    "WiFi",
    "Back"
};

// ─── Screen handles ────────────────────────────────────────────────────────────
static lv_obj_t *scr_main;
static lv_obj_t *scr_timer;
static lv_obj_t *scr_settings;
static lv_obj_t *scr_wifi;
static lv_obj_t *scr_wifi_ap;

enum UiScreen { UI_MAIN, UI_TIMER, UI_SETTINGS, UI_WIFI, UI_WIFI_AP, UI_SCHED };
static UiScreen cur_screen = UI_MAIN;

// ─── Brew + shot tracking ──────────────────────────────────────────────────────
static bool     was_brew       = false;
static uint32_t brew_start_ms  = 0;
static uint32_t brew_end_ms    = 0;
static bool     returning_brew = false;
// shot_count lives in settings.shot_count (persisted to NVS)
static bool     bbw_stop_fired = false;

// ─── Demo mode ────────────────────────────────────────────────────────────────
static bool     demo_brew       = false;
static uint32_t demo_brew_start = 0;
#define DEMO_BREW_DUR_MS 30000UL

static bool get_brew_active() {
    return machine.connected ? machine.brew_active : demo_brew;
}

static float get_display_weight() {
    if (scale_connected()) return scale_weight();
    if (!demo_brew) return 0.0f;
    float elapsed = (millis() - brew_start_ms) / 1000.0f;
    float ramp    = (elapsed / (DEMO_BREW_DUR_MS / 1000.0f)) * settings.brew_target_g;
    return ramp < settings.brew_target_g ? ramp : settings.brew_target_g;
}

static void demo_update() {
    if (demo_brew && (millis() - demo_brew_start) >= DEMO_BREW_DUR_MS)
        demo_brew = false;
}

// ─── MAIN SCREEN ──────────────────────────────────────────────────────────────

static lv_obj_t *lbl_temp;
static lv_obj_t *obj_steam;
static lv_obj_t *lbl_steam;
static lv_obj_t *obj_heat;    // top-left pill, lit while the boiler element is on
static lv_obj_t *lbl_heat;
static lv_obj_t *lbl_brew;
static lv_obj_t *lbl_shots;
static lv_obj_t *lbl_scale_weight;
static lv_obj_t *led_status;   // connectivity dot: green = WiFi + MQTT up
static lv_obj_t *lbl_hint;
static lv_obj_t *obj_sleep_overlay;   // scheduled-standby cover: tap to wake
static lv_obj_t *lbl_sleep_sub;
static lv_obj_t *lbl_target_val;
static lv_obj_t *lbl_settemp_val;
static lv_obj_t *obj_clean;
static lv_obj_t *lbl_clean;
static lv_obj_t *obj_clean_overlay;
static lv_obj_t *lbl_clean_overlay_title;
static lv_obj_t *lbl_clean_overlay_sub;
static lv_obj_t *lbl_clean_count;
static lv_obj_t *obj_clean_stop;

// Cleaning-cycle UI state machine (drives the overlay through the process).
// The LM backflush runs CLEAN_PHASES pump phases (~5 s pump + ~10 s pause).
enum CleanStage { CLEAN_IDLE, CLEAN_PREP, CLEAN_WAIT_LEVER, CLEAN_RUNNING, CLEAN_DONE };
static const int  CLEAN_PHASES = 10;   // user-counted on the real machine
static CleanStage s_clean_stage        = CLEAN_IDLE;
static bool       s_clean_pump_prev    = false;
static uint32_t   s_clean_run_start_ms = 0;
// The Z-stream flags the WHOLE cycle as one continuous shot (z_shot_active
// stays set through the pump pauses — observed live 2026-07-03), so phases
// can't be counted from telemetry. The cycle is deterministic (~5 × 16 s),
// so the countdown is time-based; the z_shot_active falling edge is the
// real end-of-cycle signal and completes the overlay automatically.
static const uint32_t CLEAN_PHASE_MS = 8000;   // 10 phases over the ~80 s cycle

// Edge adjusters (Main): NVS writes are debounced so a burst of arrow taps
// commits once, 3 s after the last tap (see ui_tick()). The set-temp commit
// also sends the new setpoint to the machine at that point — never per-tap.
static uint32_t s_target_touched_ms  = 0;
static uint32_t s_settemp_touched_ms = 0;

// CLEAN hold-to-start state (see ui_tick()).
static uint32_t s_clean_press_ms = 0;

static void target_show() {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f g", settings.brew_target_g);
    lv_label_set_text(lbl_target_val, buf);
}

static void target_adjust(float delta) {
    settings.brew_target_g += delta;
    if (settings.brew_target_g < 20.0f) settings.brew_target_g = 20.0f;
    if (settings.brew_target_g > 60.0f) settings.brew_target_g = 60.0f;
    s_target_touched_ms = millis();
    target_show();
}

static void target_up_cb(lv_event_t *e)   { target_adjust(+1.0f); }
static void target_down_cb(lv_event_t *e) { target_adjust(-1.0f); }

static void settemp_show() {
    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f\xc2\xb0", settings.coffee_temp_c);
    lv_label_set_text(lbl_settemp_val, buf);
}

static void settemp_adjust(float delta) {
    settings.coffee_temp_c += delta;
    if (settings.coffee_temp_c < 88.0f) settings.coffee_temp_c = 88.0f;
    if (settings.coffee_temp_c > 96.0f) settings.coffee_temp_c = 96.0f;
    s_settemp_touched_ms = millis();
    settemp_show();
}

static void settemp_up_cb(lv_event_t *e)   { settemp_adjust(+0.5f); }
static void settemp_down_cb(lv_event_t *e) { settemp_adjust(-0.5f); }

// CLEAN pill: PRESSED starts the 2 s hold window (checked in ui_tick());
// releasing early cancels. The pill turns amber while armed.
static void clean_press_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        s_clean_press_ms = millis();
        lv_obj_set_style_bg_color(obj_clean, lv_color_make(0xD4, 0x89, 0x1A), 0);
        lv_obj_set_style_text_color(lbl_clean, lv_color_black(), 0);
    } else {  // RELEASED / PRESS_LOST
        s_clean_press_ms = 0;
        lv_obj_set_style_bg_color(obj_clean, lv_color_make(0x28, 0x28, 0x28), 0);
        lv_obj_set_style_text_color(lbl_clean, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    }
}

static void clean_overlay_click(lv_event_t *e) {
    switch (s_clean_stage) {
        case CLEAN_PREP:
        case CLEAN_WAIT_LEVER:   // tap = cancel before the cycle starts
            machine_clean_stop();
            s_clean_stage = CLEAN_IDLE;
            lv_obj_add_flag(obj_clean_overlay, LV_OBJ_FLAG_HIDDEN);
            break;
        case CLEAN_DONE:         // tap = close the completion message
            s_clean_stage = CLEAN_IDLE;
            lv_obj_add_flag(obj_clean_overlay, LV_OBJ_FLAG_HIDDEN);
            break;
        default:                 // mid-cycle: only the STOP button acts
            break;
    }
}

static void clean_show_done(const char* title) {
    machine_clean_stop();
    s_clean_stage = CLEAN_DONE;
    lv_label_set_text(lbl_clean_overlay_title, title);
    lv_label_set_text(lbl_clean_overlay_sub,
                      "Lower the brew lever.\nTap to close.");
    lv_obj_add_flag(lbl_clean_count, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(obj_clean_stop, LV_OBJ_FLAG_HIDDEN);
}

static void clean_stop_btn_cb(lv_event_t *e) {
    clean_show_done("Cleaning stopped");
}

static void ui_main_create() {
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_black(), 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_main, 0, 0);

    // Actual temperature, bold, centred between the two edge adjusters.
    lbl_temp = lv_label_create(scr_main);
    lv_label_set_text(lbl_temp, "--.-\xc2\xb0" "C");
    lv_obj_set_style_text_font(lbl_temp, &lv_font_lm72_bold, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl_temp, LV_ALIGN_TOP_MID, 0, 44);

    obj_steam = lv_obj_create(scr_main);
    lv_obj_set_size(obj_steam, 120, 32);
    lv_obj_align(obj_steam, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(obj_steam, 16, 0);
    lv_obj_set_style_border_width(obj_steam, 0, 0);
    lv_obj_set_style_bg_color(obj_steam, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_pad_all(obj_steam, 0, 0);
    lv_obj_clear_flag(obj_steam, LV_OBJ_FLAG_SCROLLABLE);
    lbl_steam = lv_label_create(obj_steam);
    lv_label_set_text(lbl_steam, "STEAM");
    lv_obj_set_style_text_font(lbl_steam, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_steam, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_align(lbl_steam, LV_ALIGN_CENTER, 0, 0);

    // HEAT: mirrors the STEAM pill on the left edge; lit while the machine
    // reports the boiler heating element active (boiler_flags bit 0).
    obj_heat = lv_obj_create(scr_main);
    lv_obj_set_size(obj_heat, 120, 32);
    lv_obj_align(obj_heat, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(obj_heat, 16, 0);
    lv_obj_set_style_border_width(obj_heat, 0, 0);
    lv_obj_set_style_bg_color(obj_heat, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_pad_all(obj_heat, 0, 0);
    lv_obj_clear_flag(obj_heat, LV_OBJ_FLAG_SCROLLABLE);
    lbl_heat = lv_label_create(obj_heat);
    lv_label_set_text(lbl_heat, "HEAT");
    lv_obj_set_style_text_font(lbl_heat, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_heat, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_align(lbl_heat, LV_ALIGN_CENTER, 0, 0);

    lbl_brew = lv_label_create(scr_main);
    lv_label_set_text(lbl_brew, "BREWING");
    lv_obj_set_style_text_font(lbl_brew, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_brew, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_align(lbl_brew, LV_ALIGN_CENTER, 0, 18);
    lv_obj_add_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);

    lbl_shots = lv_label_create(scr_main);
    lv_label_set_text(lbl_shots, "0 shots");
    lv_obj_set_style_text_font(lbl_shots, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_shots, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_shots, LV_ALIGN_CENTER, 0, 52);

    lbl_scale_weight = lv_label_create(scr_main);
    lv_label_set_text(lbl_scale_weight, "");
    lv_obj_set_style_text_font(lbl_scale_weight, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_scale_weight, lv_color_make(0x3A, 0x80, 0x3A), 0);
    lv_obj_align(lbl_scale_weight, LV_ALIGN_CENTER, 0, 82);

    led_status = lv_obj_create(scr_main);
    lv_obj_set_size(led_status, 16, 16);
    lv_obj_set_style_radius(led_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led_status, lv_color_make(0x35, 0x35, 0x35), 0);
    lv_obj_set_style_border_width(led_status, 0, 0);
    lv_obj_set_style_pad_all(led_status, 0, 0);
    lv_obj_clear_flag(led_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(led_status, LV_ALIGN_BOTTOM_LEFT, 8, -8);

    // Edge adjusters: ▲ / value / ▼ columns flush against each screen border,
    // vertically centred. Left = coffee setpoint (0.5° steps), right = brew
    // target weight (1 g steps).
    auto make_arrow = [&](const char* sym, lv_event_cb_t cb, lv_align_t align, int x_ofs, int y_ofs) {
        lv_obj_t *btn = lv_obj_create(scr_main);
        lv_obj_set_size(btn, 64, 44);
        lv_obj_align(btn, align, x_ofs, y_ofs);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x28, 0x28, 0x28), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, sym);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_make(0xD4, 0x89, 0x1A), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_col_val = [&](lv_align_t align, int x_ofs) {
        lv_obj_t *lbl = lv_label_create(scr_main);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_make(0xCC, 0xCC, 0xCC), 0);
        lv_obj_set_width(lbl, 64);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, align, x_ofs, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    };

    make_arrow(LV_SYMBOL_UP,   target_up_cb,    LV_ALIGN_RIGHT_MID, -8, -64);
    make_arrow(LV_SYMBOL_DOWN, target_down_cb,  LV_ALIGN_RIGHT_MID, -8,  64);
    lbl_target_val = make_col_val(LV_ALIGN_RIGHT_MID, -8);
    target_show();

    make_arrow(LV_SYMBOL_UP,   settemp_up_cb,   LV_ALIGN_LEFT_MID, 8, -64);
    make_arrow(LV_SYMBOL_DOWN, settemp_down_cb, LV_ALIGN_LEFT_MID, 8,  64);
    lbl_settemp_val = make_col_val(LV_ALIGN_LEFT_MID, 8);
    settemp_show();

    // CLEAN: hold 2 s to arm the machine's cleaning cycle (fires in ui_tick()).
    obj_clean = lv_obj_create(scr_main);
    lv_obj_set_size(obj_clean, 120, 36);
    lv_obj_align(obj_clean, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_radius(obj_clean, 18, 0);
    lv_obj_set_style_border_width(obj_clean, 0, 0);
    lv_obj_set_style_bg_color(obj_clean, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_pad_all(obj_clean, 0, 0);
    lv_obj_clear_flag(obj_clean, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_clean, clean_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj_clean, clean_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj_clean, clean_press_cb, LV_EVENT_PRESS_LOST, NULL);
    lbl_clean = lv_label_create(obj_clean);
    lv_label_set_text(lbl_clean, "CLEAN");
    lv_obj_set_style_text_font(lbl_clean, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_clean, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_clean, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(lbl_clean, LV_OBJ_FLAG_CLICKABLE);

    // Lever-reminder overlay shown once the CLEAN hold completes. Tap to
    // dismiss; also auto-dismisses when the cycle starts (see ui_tick()).
    obj_clean_overlay = lv_obj_create(scr_main);
    lv_obj_set_size(obj_clean_overlay, 480, 320);
    lv_obj_set_pos(obj_clean_overlay, 0, 0);
    lv_obj_set_style_bg_color(obj_clean_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj_clean_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(obj_clean_overlay, 0, 0);
    lv_obj_set_style_radius(obj_clean_overlay, 0, 0);
    lv_obj_clear_flag(obj_clean_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_clean_overlay, clean_overlay_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(obj_clean_overlay, LV_OBJ_FLAG_HIDDEN);

    lbl_clean_overlay_title = lv_label_create(obj_clean_overlay);
    lv_label_set_text(lbl_clean_overlay_title, "Cleaning cycle");
    lv_obj_set_style_text_font(lbl_clean_overlay_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_clean_overlay_title, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl_clean_overlay_title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(lbl_clean_overlay_title, LV_OBJ_FLAG_CLICKABLE);

    // Big cycles-remaining count, dead centre (visible only while running).
    lbl_clean_count = lv_label_create(obj_clean_overlay);
    lv_label_set_text(lbl_clean_count, "5");
    lv_obj_set_style_text_font(lbl_clean_count, &lv_font_lm72_bold, 0);
    lv_obj_set_style_text_color(lbl_clean_count, lv_color_white(), 0);
    lv_obj_align(lbl_clean_count, LV_ALIGN_CENTER, 0, -6);
    lv_obj_clear_flag(lbl_clean_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl_clean_count, LV_OBJ_FLAG_HIDDEN);

    lbl_clean_overlay_sub = lv_label_create(obj_clean_overlay);
    lv_label_set_text(lbl_clean_overlay_sub,
                      "Preparing the machine...\nKeep the lever down.");
    lv_obj_set_style_text_font(lbl_clean_overlay_sub, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_clean_overlay_sub, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_align(lbl_clean_overlay_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_clean_overlay_sub, LV_ALIGN_CENTER, 0, 60);
    lv_obj_clear_flag(lbl_clean_overlay_sub, LV_OBJ_FLAG_CLICKABLE);

    // STOP button (visible only while the cycle is running).
    obj_clean_stop = lv_obj_create(obj_clean_overlay);
    lv_obj_set_size(obj_clean_stop, 160, 44);
    lv_obj_align(obj_clean_stop, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(obj_clean_stop, 22, 0);
    lv_obj_set_style_border_width(obj_clean_stop, 0, 0);
    lv_obj_set_style_bg_color(obj_clean_stop, lv_color_make(0xB0, 0x2A, 0x25), 0);
    lv_obj_set_style_pad_all(obj_clean_stop, 0, 0);
    lv_obj_clear_flag(obj_clean_stop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_clean_stop, clean_stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(obj_clean_stop, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *sl = lv_label_create(obj_clean_stop);
        lv_label_set_text(sl, "STOP");
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(sl, lv_color_white(), 0);
        lv_obj_align(sl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(sl, LV_OBJ_FLAG_CLICKABLE);
    }

    // Repurposed as Gicar diagnostic line (always visible, colour-coded)
    lbl_hint = lv_label_create(scr_main);
    lv_label_set_text(lbl_hint, "Gicar: waiting...");
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hint, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_LEFT, 32, -6);   // right of the status dot


    // MENU: bottom-centre box; LONG press opens the settings screen (a plain
    // tap does nothing — replaces the old tap-anywhere-on-screen behaviour).
    lv_obj_t *obj_menu = lv_obj_create(scr_main);
    lv_obj_set_size(obj_menu, 120, 36);
    lv_obj_align(obj_menu, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(obj_menu, 18, 0);
    lv_obj_set_style_border_width(obj_menu, 0, 0);
    lv_obj_set_style_bg_color(obj_menu, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_pad_all(obj_menu, 0, 0);
    lv_obj_clear_flag(obj_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_menu, menu_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *lbl_menu = lv_label_create(obj_menu);
    lv_label_set_text(lbl_menu, "MENU");
    lv_obj_set_style_text_font(lbl_menu, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_menu, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(lbl_menu, LV_OBJ_FLAG_CLICKABLE);

    // Sleeping cover: shown while machine.standby is set (schedule or HA).
    // Created last so it sits above every other main-screen widget.
    obj_sleep_overlay = lv_obj_create(scr_main);
    lv_obj_set_size(obj_sleep_overlay, 480, 320);
    lv_obj_set_pos(obj_sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(obj_sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj_sleep_overlay, 0, 0);
    lv_obj_set_style_radius(obj_sleep_overlay, 0, 0);
    lv_obj_clear_flag(obj_sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(obj_sleep_overlay, sleep_wake_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *t = lv_label_create(obj_sleep_overlay);
        lv_label_set_text(t, "SLEEPING");
        lv_obj_set_style_text_font(t, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(t, lv_color_make(0xCC, 0xCC, 0xCC), 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, -36);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }
    lbl_sleep_sub = lv_label_create(obj_sleep_overlay);
    lv_label_set_text(lbl_sleep_sub, "tap anywhere to wake");
    lv_obj_set_style_text_font(lbl_sleep_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_sleep_sub, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_sleep_sub, LV_ALIGN_CENTER, 0, 24);
    lv_obj_clear_flag(lbl_sleep_sub, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_clear_flag(obj_steam,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_steam,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj_heat,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_heat,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_temp,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_brew,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_shots,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_scale_weight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(led_status,       LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_hint,         LV_OBJ_FLAG_CLICKABLE);
}

static void ui_main_update() {
    char tbuf[20];
    if (machine.connected) {
        snprintf(tbuf, sizeof(tbuf), "%.1f\xc2\xb0" "C", machine.coffee_temp_c);
        lv_obj_set_style_text_color(lbl_temp, lv_color_make(0xD4, 0x89, 0x1A), 0);
    } else {
        snprintf(tbuf, sizeof(tbuf), "---\xc2\xb0" "C");
        lv_obj_set_style_text_color(lbl_temp, lv_color_make(0x70, 0x70, 0x70), 0);
    }
    lv_label_set_text(lbl_temp, tbuf);

    bool steam = machine.connected ? machine.steam_active : settings.steam_on;
    lv_obj_set_style_bg_color(obj_steam,
        steam ? lv_color_make(0xFF, 0x70, 0x43) : lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_text_color(lbl_steam,
        steam ? lv_color_white() : lv_color_make(0x70, 0x70, 0x70), 0);

    bool heat = machine.connected && machine.heating_element;
    lv_obj_set_style_bg_color(obj_heat,
        heat ? lv_color_make(0xE5, 0x39, 0x35) : lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_text_color(lbl_heat,
        heat ? lv_color_white() : lv_color_make(0x70, 0x70, 0x70), 0);

    if (get_brew_active())
        lv_obj_clear_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%u shots", (unsigned)settings.shot_count);
    lv_label_set_text(lbl_shots, sbuf);

    target_show();   // tracks changes made on the Settings screen too
    settemp_show();

    // CLEAN is hidden while a brew is running (and while the machine is away).
    if (get_brew_active())
        lv_obj_add_flag(obj_clean, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(obj_clean, LV_OBJ_FLAG_HIDDEN);

    if (scale_connected()) {
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "%.1f g", scale_weight());
        lv_label_set_text(lbl_scale_weight, wbuf);
        lv_obj_set_style_text_color(lbl_scale_weight, lv_color_make(0x3A, 0x80, 0x3A), 0);
    } else {
        lv_label_set_text(lbl_scale_weight, "");
    }

    // ── Connectivity dot: green = WiFi + MQTT + machine all up; amber = network
    // up but machine off/unreachable; dark = network down.
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    bool mqtt_ok = mqtt_connected();
    lv_color_t dot;
    if (wifi_ok && mqtt_ok && machine.connected)
        dot = lv_color_make(0x2E, 0xC8, 0x4B);
    else if (wifi_ok && mqtt_ok)
        dot = lv_color_make(0xD4, 0x89, 0x1A);
    else
        dot = lv_color_make(0x35, 0x35, 0x35);
    lv_obj_set_style_bg_color(led_status, dot, 0);

    // FW version, always visible next to the dot.
    lv_label_set_text(lbl_hint, FW_VERSION);

    // Scheduled/commanded standby: cover the screen with the sleeping overlay.
    if (machine.standby) {
        if (settings.sched_enabled) {
            char sb[48];
            snprintf(sb, sizeof(sb), "wakes at %02u:%02u  -  tap anywhere to wake",
                     settings.sched_wake_min / 60, settings.sched_wake_min % 60);
            lv_label_set_text(lbl_sleep_sub, sb);
        } else {
            lv_label_set_text(lbl_sleep_sub, "tap anywhere to wake");
        }
        lv_obj_clear_flag(obj_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── TIMER SCREEN ─────────────────────────────────────────────────────────────

static lv_obj_t *lbl_shot_time;
static lv_obj_t *lbl_timer_temp;
static lv_obj_t *lbl_shot_label;
static lv_obj_t *lbl_weight;
static lv_obj_t *weight_bar;
static lv_obj_t *lbl_target_weight;
static lv_obj_t *lbl_timer_shots;

static void ui_timer_create() {
    scr_timer = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_timer, lv_color_black(), 0);
    lv_obj_clear_flag(scr_timer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_timer, 0, 0);

    lbl_shot_time = lv_label_create(scr_timer);
    lv_label_set_text(lbl_shot_time, "0.0 s");
    lv_obj_set_style_text_font(lbl_shot_time, &lv_font_lm72_bold, 0);
    lv_obj_set_style_text_color(lbl_shot_time, lv_color_white(), 0);
    lv_obj_align(lbl_shot_time, LV_ALIGN_LEFT_MID, 8, -12);

    // Temp label — repositioned dynamically in update (M20/RIGHT_MID without scale,
    // M16/TOP_RIGHT with scale to avoid overlapping the weight display)
    lbl_timer_temp = lv_label_create(scr_timer);
    lv_label_set_text(lbl_timer_temp, "--.-\xc2\xb0" "C");
    lv_obj_set_style_text_font(lbl_timer_temp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_timer_temp, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl_timer_temp, LV_ALIGN_RIGHT_MID, -8, -12);

    lbl_shot_label = lv_label_create(scr_timer);
    lv_label_set_text(lbl_shot_label, "SHOT");
    lv_obj_set_style_text_font(lbl_shot_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_shot_label, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_align(lbl_shot_label, LV_ALIGN_RIGHT_MID, -8, 16);

    // Scale-only widgets — hidden until a scale connects
    lbl_weight = lv_label_create(scr_timer);
    lv_label_set_text(lbl_weight, "0.0 g");
    lv_obj_set_style_text_font(lbl_weight, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_weight, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_align(lbl_weight, LV_ALIGN_RIGHT_MID, -8, -20);
    lv_obj_add_flag(lbl_weight, LV_OBJ_FLAG_HIDDEN);

    weight_bar = lv_bar_create(scr_timer);
    lv_obj_set_size(weight_bar, 200, 12);
    lv_obj_align(weight_bar, LV_ALIGN_RIGHT_MID, -8, 20);
    lv_obj_set_style_bg_color(weight_bar, lv_color_make(0x28, 0x28, 0x28), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(weight_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(weight_bar, lv_color_make(0x5C, 0xB8, 0x5C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(weight_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(weight_bar, 0, (int)(settings.brew_target_g * 10));
    lv_bar_set_value(weight_bar, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(weight_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(weight_bar, LV_OBJ_FLAG_HIDDEN);

    lbl_target_weight = lv_label_create(scr_timer);
    lv_label_set_text(lbl_target_weight, "target 36g  (-3.0g)");
    lv_obj_set_style_text_font(lbl_target_weight, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_target_weight, lv_color_make(0x90, 0x90, 0x90), 0);
    lv_obj_align(lbl_target_weight, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_add_flag(lbl_target_weight, LV_OBJ_FLAG_HIDDEN);

    lbl_timer_shots = lv_label_create(scr_timer);
    lv_label_set_text(lbl_timer_shots, "Shot #0");
    lv_obj_set_style_text_font(lbl_timer_shots, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_timer_shots, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_align(lbl_timer_shots, LV_ALIGN_BOTTOM_LEFT, 8, -6);

    lv_obj_clear_flag(lbl_shot_time,     LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_timer_temp,    LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_shot_label,    LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_weight,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(weight_bar,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_target_weight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_timer_shots,   LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(scr_timer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_timer, timer_click_cb, LV_EVENT_CLICKED, NULL);
}

static void ui_timer_update() {
    // Freeze the displayed time at the shot's final duration once brewing has
    // stopped, instead of continuing to count up through the 3 s grace period
    // before the screen returns to Main (see returning_brew in ui_tick()).
    float elapsed = get_brew_active()
        ? (millis() - brew_start_ms) / 1000.0f
        : (brew_end_ms - brew_start_ms) / 1000.0f;
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%.1f s", elapsed);
    lv_label_set_text(lbl_shot_time, tbuf);

    char tempbuf[16];
    if (machine.connected) {
        snprintf(tempbuf, sizeof(tempbuf), "%.1f\xc2\xb0" "C", machine.coffee_temp_c);
    } else {
        snprintf(tempbuf, sizeof(tempbuf), "---\xc2\xb0" "C");
    }
    lv_label_set_text(lbl_timer_temp, tempbuf);

    if (scale_connected()) {
        // Scale mode: weight + bar in right panel, temp shrinks to top-right corner
        lv_obj_set_style_text_font(lbl_timer_temp, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_timer_temp, LV_ALIGN_TOP_RIGHT, -8, 6);

        lv_obj_add_flag(lbl_shot_label,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_weight,       LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(weight_bar,       LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_target_weight, LV_OBJ_FLAG_HIDDEN);

        float w = scale_weight();
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "%.1f g", w);
        lv_label_set_text(lbl_weight, wbuf);

        lv_bar_set_range(weight_bar, 0, (int)(settings.brew_target_g * 10));
        lv_bar_set_value(weight_bar, (int)(w * 10), LV_ANIM_OFF);

        char targetbuf[32];
        snprintf(targetbuf, sizeof(targetbuf), "target %.0fg  (-%.1fg)",
                 settings.brew_target_g, settings.prestop_offset_g);
        lv_label_set_text(lbl_target_weight, targetbuf);

        float ratio = (settings.brew_target_g > 0) ? (w / settings.brew_target_g) : 0;
        lv_color_t bar_col = (ratio >= 0.9f) ?
            lv_color_make(0xEF, 0x53, 0x50) : lv_color_make(0x5C, 0xB8, 0x5C);
        lv_obj_set_style_bg_color(weight_bar, bar_col, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(lbl_weight, bar_col, 0);
    } else {
        // No scale: clean timer layout — large temp + SHOT label on right
        lv_obj_set_style_text_font(lbl_timer_temp, &lv_font_montserrat_20, 0);
        lv_obj_align(lbl_timer_temp, LV_ALIGN_RIGHT_MID, -8, -12);

        lv_obj_clear_flag(lbl_shot_label,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_weight,         LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(weight_bar,         LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_target_weight,  LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── SETTINGS SCREEN ──────────────────────────────────────────────────────────

#define SETTINGS_HDR_H  36
#define SETTINGS_ITEM_H 36

static lv_obj_t *lbl_settings_hdr;
static lv_obj_t *settings_scroll_cont;
static lv_obj_t *settings_rows[SI_COUNT];
static lv_obj_t *settings_name_lbl[SI_COUNT];
static lv_obj_t *settings_val_lbl[SI_COUNT];
static int       settings_sel  = 0;
static bool      settings_edit = false;

static const char* get_item_val(int i) {
    static char buf[28];
    switch (i) {
        case SI_PREINFUSION:
            if (settings.preinfusion_s < 0.05f) snprintf(buf, sizeof(buf), "OFF");
            else snprintf(buf, sizeof(buf), "%.1f s", settings.preinfusion_s);
            break;
        case SI_STEAM:
            snprintf(buf, sizeof(buf), "%s", settings.steam_on ? "ON" : "OFF"); break;
        case SI_STANDBY:
            if (settings.standby_min == 0) snprintf(buf, sizeof(buf), "OFF");
            else snprintf(buf, sizeof(buf), "%d min", settings.standby_min);
            break;
        case SI_SCHEDULE:
            if (!settings.sched_enabled) snprintf(buf, sizeof(buf), "OFF");
            else snprintf(buf, sizeof(buf), "%02u:%02u - %02u:%02u",
                          settings.sched_wake_min / 60, settings.sched_wake_min % 60,
                          settings.sched_sleep_min / 60, settings.sched_sleep_min % 60);
            break;
        case SI_TIMEZONE: {
            int m = settings.tz_offset_min;
            snprintf(buf, sizeof(buf), "UTC%+03d:%02d", m / 60, abs(m) % 60);
            break;
        }
        case SI_WIFI:
            snprintf(buf, sizeof(buf), "%.18s", wifi_config_ssid()); break;
        case SI_BREW_WEIGHT:
            snprintf(buf, sizeof(buf), "%.0f g", settings.brew_target_g); break;
        case SI_PRESTOP:
            snprintf(buf, sizeof(buf), "%.1f g", settings.prestop_offset_g); break;
        case SI_BACK:
            buf[0] = '\0'; break;
        default:
            buf[0] = '\0'; break;
    }
    return buf;
}

static void settings_draw() {
    for (int i = 0; i < SI_COUNT; i++) {
        bool sel = (i == settings_sel);
        bool ed  = sel && settings_edit;

        lv_obj_set_style_bg_opa(settings_rows[i], sel ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(settings_rows[i], lv_color_make(0x28, 0x78, 0xD8), 0);

        char nbuf[32];
        snprintf(nbuf, sizeof(nbuf), "%s %s", sel ? ">" : " ", item_names[i]);
        lv_label_set_text(settings_name_lbl[i], nbuf);
        lv_obj_set_style_text_color(settings_name_lbl[i],
            sel ? lv_color_white() : lv_color_make(0x90, 0x90, 0x90), 0);

        char vbuf[32];
        bool is_value_item = (i != SI_WIFI && i != SI_BACK && i != SI_SCHEDULE);
        if (ed && is_value_item) {
            snprintf(vbuf, sizeof(vbuf), "< %s >", get_item_val(i));
            lv_obj_set_style_text_color(settings_val_lbl[i], lv_color_make(0xF0, 0xA8, 0x30), 0);
        } else {
            snprintf(vbuf, sizeof(vbuf), "%s", get_item_val(i));
            lv_obj_set_style_text_color(settings_val_lbl[i],
                sel ? lv_color_make(0xD4, 0x89, 0x1A) : lv_color_make(0x80, 0x80, 0x80), 0);
        }
        lv_label_set_text(settings_val_lbl[i], vbuf);
    }
}

static void ui_settings_create() {
    scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_settings, lv_color_black(), 0);
    lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_settings, 0, 0);

    lv_obj_t *hdr = lv_obj_create(scr_settings);
    lv_obj_set_size(hdr, 480, SETTINGS_HDR_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lbl_settings_hdr = lv_label_create(hdr);
    lv_label_set_text(lbl_settings_hdr, "SETTINGS");
    lv_obj_set_style_text_font(lbl_settings_hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_settings_hdr, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_settings_hdr, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *lbl_hdr_hint = lv_label_create(hdr);
    lv_label_set_text(lbl_hdr_hint, "btn:short=next  long=select  tap=cycle");
    lv_obj_set_style_text_font(lbl_hdr_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_hdr_hint, lv_color_make(0x60, 0x60, 0x60), 0);
    lv_obj_align(lbl_hdr_hint, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_settings_hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_hdr_hint, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *scroll = lv_obj_create(scr_settings);
    lv_obj_set_size(scroll, 480, 320 - SETTINGS_HDR_H);
    lv_obj_set_pos(scroll, 0, SETTINGS_HDR_H);
    lv_obj_set_style_bg_color(scroll, lv_color_black(), 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_radius(scroll, 0, 0);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_make(0x44, 0x44, 0x44), LV_PART_SCROLLBAR);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_CLICKABLE);
    settings_scroll_cont = scroll;

    for (int i = 0; i < SI_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(scroll);
        lv_obj_set_size(row, 480, SETTINGS_ITEM_H);
        lv_obj_set_pos(row, 0, i * SETTINGS_ITEM_H);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        settings_rows[i] = row;

        settings_name_lbl[i] = lv_label_create(row);
        lv_obj_set_style_text_font(settings_name_lbl[i], &lv_font_montserrat_20, 0);
        lv_obj_align(settings_name_lbl[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_clear_flag(settings_name_lbl[i], LV_OBJ_FLAG_CLICKABLE);

        settings_val_lbl[i] = lv_label_create(row);
        lv_obj_set_style_text_font(settings_val_lbl[i], &lv_font_montserrat_20, 0);
        lv_obj_align(settings_val_lbl[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_clear_flag(settings_val_lbl[i], LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, settings_row_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);
    }

    settings_draw();
}

static void settings_scroll() {
    settings_sel = (settings_sel + 1) % SI_COUNT;
    settings_draw();
    lv_obj_scroll_to_view(settings_rows[settings_sel], LV_ANIM_OFF);
}

static void settings_select() {
    switch (settings_sel) {
        case SI_SCHEDULE:
            ui_show_sched();
            return;
        case SI_WIFI:
            ui_show_wifi();
            return;
        case SI_BACK:
            settings_save();
            settings_sel  = 0;
            settings_edit = false;
            ui_show_main();
            return;
        default:
            settings_edit = true;
            settings_draw();
            return;
    }
}

static void settings_edit_increment() {
    switch (settings_sel) {
        case SI_PREINFUSION:
            settings.preinfusion_s += 0.5f;
            if (settings.preinfusion_s > 10.0f) settings.preinfusion_s = 0.0f;
            break;
        case SI_STEAM:
            settings.steam_on = !settings.steam_on;
            break;
        case SI_STANDBY:
            if      (settings.standby_min == 0)  settings.standby_min = 15;
            else if (settings.standby_min == 15) settings.standby_min = 30;
            else if (settings.standby_min == 30) settings.standby_min = 60;
            else                                  settings.standby_min = 0;
            break;
        case SI_BREW_WEIGHT:
            settings.brew_target_g += 1.0f;
            if (settings.brew_target_g > 60.0f) settings.brew_target_g = 20.0f;
            break;
        case SI_PRESTOP:
            settings.prestop_offset_g += 0.5f;
            if (settings.prestop_offset_g > 8.0f) settings.prestop_offset_g = 0.0f;
            break;
        case SI_TIMEZONE:
            settings.tz_offset_min += 30;
            if (settings.tz_offset_min > 840) settings.tz_offset_min = -720;  // +14h..-12h
            break;
        // SI_SCHEDULE, SI_WIFI, SI_BACK: no increment (navigation items)
    }
    settings_draw();
}

static void settings_edit_confirm() {
    settings_edit = false;
    settings_save();
    if (settings_sel == SI_STEAM) machine_set_steam(settings.steam_on);
    if (settings_sel == SI_TIMEZONE)  // re-apply the SNTP offset immediately
        configTime(settings.tz_offset_min * 60L, 0, "pool.ntp.org", "time.nist.gov");
    settings_draw();
}

// ─── WIFI STATUS SCREEN ───────────────────────────────────────────────────────

static lv_obj_t *lbl_wifi_status;
static lv_obj_t *lbl_wifi_ssid;
static lv_obj_t *lbl_wifi_ip;
static lv_obj_t *lbl_wifi_rssi;

static void wifi_back_cb(lv_event_t *e)   { ui_show_settings(); }
static void wifi_ap_btn_cb(lv_event_t *e) {
    wlogf("[ap] setup button pressed\n");
    wifi_ap_start();
    ui_show_wifi_ap();
}

static void ui_wifi_create() {
    scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi, lv_color_black(), 0);
    lv_obj_clear_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_wifi, 0, 0);

    // Header with back button
    lv_obj_t *hdr = lv_obj_create(scr_wifi);
    lv_obj_set_size(hdr, 480, 32);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hdr, wifi_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_hdr = lv_label_create(hdr);
    lv_label_set_text(lbl_hdr, "< WIFI");
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hdr, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_clear_flag(lbl_hdr, LV_OBJ_FLAG_CLICKABLE);

    // Info rows (y=28..28+4*28=140)
    auto make_row = [&](int y, const char* key, lv_obj_t** val_out) {
        lv_obj_t *lbl_k = lv_label_create(scr_wifi);
        lv_label_set_text(lbl_k, key);
        lv_obj_set_style_text_font(lbl_k, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_k, lv_color_make(0x90, 0x90, 0x90), 0);
        lv_obj_set_pos(lbl_k, 8, y + 4);
        lv_obj_clear_flag(lbl_k, LV_OBJ_FLAG_CLICKABLE);

        *val_out = lv_label_create(scr_wifi);
        lv_label_set_text(*val_out, "—");
        lv_obj_set_style_text_font(*val_out, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(*val_out, lv_color_make(0xCC, 0xCC, 0xCC), 0);
        lv_obj_align(*val_out, LV_ALIGN_TOP_RIGHT, -8, y + 4);
        lv_obj_clear_flag(*val_out, LV_OBJ_FLAG_CLICKABLE);
    };

    make_row(38,  "Status:",   &lbl_wifi_status);
    make_row(76,  "Network:",  &lbl_wifi_ssid);
    make_row(114, "IP:",       &lbl_wifi_ip);
    make_row(152, "Signal:",   &lbl_wifi_rssi);

    // AP Setup button
    lv_obj_t *btn_ap = lv_obj_create(scr_wifi);
    lv_obj_set_size(btn_ap, 260, 36);
    lv_obj_align(btn_ap, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(btn_ap, lv_color_make(0x1A, 0x6A, 0xBB), 0);
    lv_obj_set_style_border_width(btn_ap, 0, 0);
    lv_obj_set_style_radius(btn_ap, 4, 0);
    lv_obj_set_style_pad_all(btn_ap, 0, 0);
    lv_obj_clear_flag(btn_ap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_ap, wifi_ap_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn_ap);
    lv_label_set_text(btn_lbl, "Start AP Setup Mode");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(btn_lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void ui_wifi_update() {
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(lbl_wifi_status, "Connected");
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_make(0x5C, 0xB8, 0x5C), 0);
        lv_label_set_text(lbl_wifi_ssid, WiFi.SSID().c_str());
        lv_label_set_text(lbl_wifi_ip,   WiFi.localIP().toString().c_str());
        char rssi_buf[16];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", WiFi.RSSI());
        lv_label_set_text(lbl_wifi_rssi, rssi_buf);
    } else {
        lv_label_set_text(lbl_wifi_status, "Not connected");
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_make(0xEF, 0x53, 0x50), 0);
        lv_label_set_text(lbl_wifi_ssid, wifi_config_ssid());
        lv_label_set_text(lbl_wifi_ip,   "—");
        lv_label_set_text(lbl_wifi_rssi, "—");
    }
}

// ─── WIFI AP MODE SCREEN ──────────────────────────────────────────────────────

static void wifi_ap_cancel_cb(lv_event_t *e) {
    ESP.restart();  // cleanest way to exit AP mode and retry normal connection
}

static void ui_wifi_ap_create() {
    scr_wifi_ap = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi_ap, lv_color_black(), 0);
    lv_obj_clear_flag(scr_wifi_ap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_wifi_ap, 0, 0);

    lv_obj_t *lbl_title = lv_label_create(scr_wifi_ap);
    lv_label_set_text(lbl_title, "WIFI SETUP MODE");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *lbl1 = lv_label_create(scr_wifi_ap);
    lv_label_set_text(lbl1, "1. Connect phone to:");
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl1, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_pos(lbl1, 12, 56);

    lv_obj_t *lbl_ssid = lv_label_create(scr_wifi_ap);
    lv_label_set_text(lbl_ssid, WIFI_AP_SSID);
    lv_obj_set_style_text_font(lbl_ssid, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ssid, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_set_pos(lbl_ssid, 12, 82);

    lv_obj_t *lbl2 = lv_label_create(scr_wifi_ap);
    lv_label_set_text(lbl2, "2. Open browser, go to:");
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl2, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_pos(lbl2, 12, 132);

    lv_obj_t *lbl_ip = lv_label_create(scr_wifi_ap);
    lv_label_set_text(lbl_ip, WIFI_AP_IP);
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ip, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_set_pos(lbl_ip, 12, 158);

    // Cancel/restart button
    lv_obj_t *btn = lv_obj_create(scr_wifi_ap);
    lv_obj_set_size(btn, 260, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x55, 0x22, 0x22), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, wifi_ap_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Cancel (Restart)");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_make(0xCC, 0x66, 0x66), 0);
    lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(btn_lbl, LV_OBJ_FLAG_CLICKABLE);
}

// ─── STANDBY SCHEDULE SCREEN ──────────────────────────────────────────────────
// Daily absolute-time sleep/wake (settings.sched_*), edited with the same
// arrow idiom as the main screen. Times are minutes since local midnight.

static lv_obj_t *scr_sched;
static lv_obj_t *obj_sched_en, *lbl_sched_en;
static lv_obj_t *lbl_sched_wake, *lbl_sched_sleep;
static lv_obj_t *lbl_sched_next, *lbl_sched_next_cap, *lbl_sched_clock;

static void sched_wrap(uint16_t *v, int delta) {
    int n = (int)*v + delta;
    while (n < 0)     n += 1440;
    while (n >= 1440) n -= 1440;
    *v = (uint16_t)n;
}
static void sched_wake_h_up_cb(lv_event_t *e)   { sched_wrap(&settings.sched_wake_min,   60); }
static void sched_wake_h_dn_cb(lv_event_t *e)   { sched_wrap(&settings.sched_wake_min,  -60); }
static void sched_wake_m_up_cb(lv_event_t *e)   { sched_wrap(&settings.sched_wake_min,   15); }
static void sched_wake_m_dn_cb(lv_event_t *e)   { sched_wrap(&settings.sched_wake_min,  -15); }
static void sched_sleep_h_up_cb(lv_event_t *e)  { sched_wrap(&settings.sched_sleep_min,  60); }
static void sched_sleep_h_dn_cb(lv_event_t *e)  { sched_wrap(&settings.sched_sleep_min, -60); }
static void sched_sleep_m_up_cb(lv_event_t *e)  { sched_wrap(&settings.sched_sleep_min,  15); }
static void sched_sleep_m_dn_cb(lv_event_t *e)  { sched_wrap(&settings.sched_sleep_min, -15); }
static void sched_en_cb(lv_event_t *e)          { settings.sched_enabled = !settings.sched_enabled; }
static void sched_back_cb(lv_event_t *e) {
    settings_save();
    ui_show_settings();
}

static lv_obj_t* sched_arrow(const char *sym, lv_event_cb_t cb, int x, int y) {
    lv_obj_t *btn = lv_obj_create(scr_sched);
    lv_obj_set_size(btn, 64, 44);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void ui_sched_create() {
    scr_sched = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_sched, lv_color_black(), 0);
    lv_obj_clear_flag(scr_sched, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_sched, 0, 0);

    lv_obj_t *hdr = lv_label_create(scr_sched);
    lv_label_set_text(hdr, "STANDBY SCHEDULE");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(hdr, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 10);

    // ENABLED toggle pill, top-right.
    obj_sched_en = lv_obj_create(scr_sched);
    lv_obj_set_size(obj_sched_en, 116, 34);
    lv_obj_align(obj_sched_en, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_set_style_radius(obj_sched_en, 17, 0);
    lv_obj_set_style_border_width(obj_sched_en, 0, 0);
    lv_obj_set_style_pad_all(obj_sched_en, 0, 0);
    lv_obj_clear_flag(obj_sched_en, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj_sched_en, sched_en_cb, LV_EVENT_CLICKED, NULL);
    lbl_sched_en = lv_label_create(obj_sched_en);
    lv_obj_set_style_text_font(lbl_sched_en, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_sched_en, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(lbl_sched_en, LV_OBJ_FLAG_CLICKABLE);

    auto col_label = [&](const char *txt, int x) {
        lv_obj_t *l = lv_label_create(scr_sched);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_set_width(l, 144);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x, 58);
    };
    col_label("WAKE",  40);
    col_label("SLEEP", 480 - 40 - 144);

    // Wake column (left): hour arrows over minute arrows, time between.
    sched_arrow(LV_SYMBOL_UP,   sched_wake_h_up_cb,   40, 84);
    sched_arrow(LV_SYMBOL_UP,   sched_wake_m_up_cb,  120, 84);
    lbl_sched_wake = lv_label_create(scr_sched);
    lv_obj_set_style_text_font(lbl_sched_wake, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_sched_wake, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_set_width(lbl_sched_wake, 144);
    lv_obj_set_style_text_align(lbl_sched_wake, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_sched_wake, 40, 142);
    sched_arrow(LV_SYMBOL_DOWN, sched_wake_h_dn_cb,   40, 204);
    sched_arrow(LV_SYMBOL_DOWN, sched_wake_m_dn_cb,  120, 204);

    // Sleep column (right).
    sched_arrow(LV_SYMBOL_UP,   sched_sleep_h_up_cb, 480 - 40 - 144, 84);
    sched_arrow(LV_SYMBOL_UP,   sched_sleep_m_up_cb, 480 - 40 - 64,  84);
    lbl_sched_sleep = lv_label_create(scr_sched);
    lv_obj_set_style_text_font(lbl_sched_sleep, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl_sched_sleep, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_set_width(lbl_sched_sleep, 144);
    lv_obj_set_style_text_align(lbl_sched_sleep, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_sched_sleep, 480 - 40 - 144, 142);
    sched_arrow(LV_SYMBOL_DOWN, sched_sleep_h_dn_cb, 480 - 40 - 144, 204);
    sched_arrow(LV_SYMBOL_DOWN, sched_sleep_m_dn_cb, 480 - 40 - 64,  204);

    // Hour/minute captions under each arrow pair.
    auto cap = [&](const char *txt, int x) {
        lv_obj_t *l = lv_label_create(scr_sched);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, lv_color_make(0x60, 0x60, 0x60), 0);
        lv_obj_set_width(l, 64);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x, 252);
    };
    cap("hour", 40);            cap("min",  120);
    cap("hour", 480 - 40 - 144); cap("min", 480 - 40 - 64);

    // Centre: next-event countdown.
    lbl_sched_next_cap = lv_label_create(scr_sched);
    lv_label_set_text(lbl_sched_next_cap, "");
    lv_obj_set_style_text_font(lbl_sched_next_cap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_sched_next_cap, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_sched_next_cap, LV_ALIGN_CENTER, 0, -28);
    lbl_sched_next = lv_label_create(scr_sched);
    lv_label_set_text(lbl_sched_next, "");
    lv_obj_set_style_text_font(lbl_sched_next, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_sched_next, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(lbl_sched_next, LV_ALIGN_CENTER, 0, 0);

    // Footer: board clock + sync state (left), BACK (right).
    lbl_sched_clock = lv_label_create(scr_sched);
    lv_label_set_text(lbl_sched_clock, "");
    lv_obj_set_style_text_font(lbl_sched_clock, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_sched_clock, LV_ALIGN_BOTTOM_LEFT, 12, -12);

    lv_obj_t *btn_back = lv_obj_create(scr_sched);
    lv_obj_set_size(btn_back, 116, 34);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_RIGHT, -10, -8);
    lv_obj_set_style_radius(btn_back, 17, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_pad_all(btn_back, 0, 0);
    lv_obj_clear_flag(btn_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_back, sched_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn_back);
    lv_label_set_text(bl, "BACK");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(bl, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_align(bl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);
}

static void ui_sched_update() {
    char b[24];
    snprintf(b, sizeof(b), "%02u:%02u",
             settings.sched_wake_min / 60, settings.sched_wake_min % 60);
    lv_label_set_text(lbl_sched_wake, b);
    snprintf(b, sizeof(b), "%02u:%02u",
             settings.sched_sleep_min / 60, settings.sched_sleep_min % 60);
    lv_label_set_text(lbl_sched_sleep, b);

    lv_label_set_text(lbl_sched_en, settings.sched_enabled ? "ENABLED" : "OFF");
    lv_obj_set_style_bg_color(obj_sched_en,
        settings.sched_enabled ? lv_color_make(0x2E, 0x5E, 0x35)
                               : lv_color_make(0x28, 0x28, 0x28), 0);
    lv_obj_set_style_text_color(lbl_sched_en,
        settings.sched_enabled ? lv_color_white() : lv_color_make(0x70, 0x70, 0x70), 0);

    struct tm ti;
    bool clock_ok = getLocalTime(&ti, 0);
    if (clock_ok) {
        char cb[40];
        snprintf(cb, sizeof(cb), "clock %02d:%02d (NTP)", ti.tm_hour, ti.tm_min);
        lv_label_set_text(lbl_sched_clock, cb);
        lv_obj_set_style_text_color(lbl_sched_clock, lv_color_make(0x5C, 0xB8, 0x5C), 0);

        if (settings.sched_enabled) {
            int mod = ti.tm_hour * 60 + ti.tm_min;
            int to_wake  = ((int)settings.sched_wake_min  - mod + 1440) % 1440;
            int to_sleep = ((int)settings.sched_sleep_min - mod + 1440) % 1440;
            bool wake_first = to_wake < to_sleep;
            int mins = wake_first ? to_wake : to_sleep;
            lv_label_set_text(lbl_sched_next_cap, wake_first ? "wakes in" : "sleeps in");
            snprintf(cb, sizeof(cb), "%d h %02d m", mins / 60, mins % 60);
            lv_label_set_text(lbl_sched_next, cb);
        } else {
            lv_label_set_text(lbl_sched_next_cap, "");
            lv_label_set_text(lbl_sched_next, "schedule off");
        }
    } else {
        lv_label_set_text(lbl_sched_clock, "waiting for clock (NTP)");
        lv_obj_set_style_text_color(lbl_sched_clock, lv_color_make(0xD4, 0x89, 0x1A), 0);
        lv_label_set_text(lbl_sched_next_cap, "");
        lv_label_set_text(lbl_sched_next, clock_ok ? "" : "no time sync yet");
    }
}

// ─── Screen switchers ──────────────────────────────────────────────────────────

static void ui_show_main() {
    cur_screen = UI_MAIN;
    lv_scr_load(scr_main);
}

static void ui_show_timer() {
    cur_screen = UI_TIMER;
    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "Shot #%u", (unsigned)settings.shot_count);
    lv_label_set_text(lbl_timer_shots, sbuf);
    lv_bar_set_range(weight_bar, 0, (int)(settings.brew_target_g * 10));
    lv_bar_set_value(weight_bar, 0, LV_ANIM_OFF);
    lv_scr_load(scr_timer);
}

static void ui_show_settings() {
    settings_sel  = 0;
    settings_edit = false;
    lv_label_set_text(lbl_settings_hdr, "SETTINGS");
    settings_draw();
    cur_screen = UI_SETTINGS;
    lv_scr_load(scr_settings);
}

static void ui_show_wifi() {
    ui_wifi_update();
    cur_screen = UI_WIFI;
    lv_scr_load(scr_wifi);
}

static void ui_show_wifi_ap() {
    cur_screen = UI_WIFI_AP;
    lv_scr_load(scr_wifi_ap);
}

static void ui_show_sched() {
    ui_sched_update();
    cur_screen = UI_SCHED;
    lv_scr_load(scr_sched);
}

// ─── Touch callbacks ───────────────────────────────────────────────────────────

static void menu_long_press_cb(lv_event_t *e) {
    if (cur_screen == UI_MAIN) ui_show_settings();
}

static void sleep_wake_cb(lv_event_t *e) {
    machine_set_standby(false);   // manual override — holds until the next edge
    wlogf("[sched] manual wake (overlay tap)\n");
}

static void timer_click_cb(lv_event_t *e) {
    // Ignore taps while a real brew is active — resetting was_brew mid-brew would
    // re-trigger the brew-start block (double shot count, re-tare, screen bounce)
    if (machine.connected && machine.brew_active) return;
    demo_brew       = false;
    was_brew        = false;
    returning_brew  = false;
    bbw_stop_fired  = false;
    if (cur_screen == UI_TIMER) ui_show_main();
}

static void settings_row_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx == SI_WIFI) {
        ui_show_wifi();
        return;
    }
    if (idx == SI_SCHEDULE) {
        ui_show_sched();
        return;
    }
    if (idx == SI_BACK) {
        settings_save();
        settings_sel  = 0;
        settings_edit = false;
        ui_show_main();
        return;
    }
    // Value items: tap selects + cycles one step
    settings_sel  = idx;
    settings_edit = false;
    settings_edit_increment();
}

// ─── Public API ────────────────────────────────────────────────────────────────

void ui_init() {
    button_init();
    ui_main_create();
    ui_timer_create();
    ui_settings_create();
    ui_wifi_create();
    ui_wifi_ap_create();
    ui_sched_create();
    lv_scr_load(scr_main);
}

void ui_tick() {
    demo_update();
    button_update();

    // Debounced NVS commit for the Main-screen target-weight arrows.
    if (s_target_touched_ms != 0 && millis() - s_target_touched_ms >= 3000) {
        s_target_touched_ms = 0;
        settings_save();
    }
    // Set-temp arrows: commit to NVS and send the new setpoint to the machine
    // once, 3 s after the last tap — never per-tap (each send is a real GICAR
    // write to the boiler controller).
    if (s_settemp_touched_ms != 0 && millis() - s_settemp_touched_ms >= 3000) {
        s_settemp_touched_ms = 0;
        settings_save();
        machine_set_temp(settings.coffee_temp_c);
    }

    bool brew_now = get_brew_active();

    // CLEAN hold-to-start: fires once the pill has been held for 2 s.
    if (s_clean_press_ms != 0 && millis() - s_clean_press_ms >= 2000 && !brew_now) {
        s_clean_press_ms = 0;
        lv_obj_set_style_bg_color(obj_clean, lv_color_make(0x28, 0x28, 0x28), 0);
        lv_obj_set_style_text_color(lbl_clean, lv_color_make(0xCC, 0xCC, 0xCC), 0);
        machine_clean_start();
        time_t now = time(nullptr);
        if (now > 1577836800UL) {  // sanity: after 2020-01-01
            settings.last_cleaning_epoch = (uint32_t)now;
            settings_save();
        }
        s_clean_stage = CLEAN_PREP;
        lv_label_set_text(lbl_clean_overlay_title, "Cleaning cycle");
        lv_label_set_text(lbl_clean_overlay_sub,
                          "Preparing the machine...\nKeep the lever down.");
        lv_obj_add_flag(lbl_clean_count, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(obj_clean_stop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(obj_clean_overlay, LV_OBJ_FLAG_HIDDEN);
        wlogf("[ui] clean cycle armed (2s hold)\n");
    }

    // ── Standby schedule: fire once at each sleep/wake edge ─────────────────────
    // Edge-triggered on the minute so a manual wake (or HA command) between the
    // edges is never fought. Sleep is skipped while a brew or cleaning cycle is
    // running and retried each minute for up to 15 min after the edge.
    {
        static int s_last_mod = -1;
        struct tm ti;
        if (settings.sched_enabled && getLocalTime(&ti, 0)) {
            int mod = ti.tm_hour * 60 + ti.tm_min;
            if (mod != s_last_mod) {
                bool booted_mid_run = (s_last_mod == -1);
                s_last_mod = mod;
                if (!booted_mid_run) {
                    int past_sleep = (mod - (int)settings.sched_sleep_min + 1440) % 1440;
                    if (past_sleep < 15 && !machine.standby &&
                        !get_brew_active() && !machine_clean_active()) {
                        // fire only right at the edge; the <15 window is the
                        // busy-retry, and a manual wake inside it is respected
                        // because machine.standby was true at the edge itself
                        static uint32_t s_sleep_fired_day = 0;
                        uint32_t day_key = (uint32_t)(ti.tm_yday + 1);
                        if (s_sleep_fired_day != day_key) {
                            s_sleep_fired_day = day_key;
                            machine_set_standby(true);
                            wlogf("[sched] sleep %02d:%02d\n", ti.tm_hour, ti.tm_min);
                        }
                    }
                    if (mod == (int)settings.sched_wake_min && machine.standby) {
                        machine_set_standby(false);
                        wlogf("[sched] wake %02d:%02d\n", ti.tm_hour, ti.tm_min);
                    }
                }
            }
        }
    }

    // ── Cleaning-cycle overlay state machine ────────────────────────────────────
    switch (s_clean_stage) {
        case CLEAN_PREP:
            // Burst finished — now (and only now) invite the lever. Lifting it
            // during the burst poisons the cycle (pulses landing mid-cycle
            // abort it after one phase).
            if (machine_clean_ready()) {
                s_clean_stage = CLEAN_WAIT_LEVER;
                lv_label_set_text(lbl_clean_overlay_sub,
                                  "Lift the brew lever to start.\nTap to cancel.");
            }
            break;
        case CLEAN_WAIT_LEVER:
            // Z-frames react within ~100 ms; the R-frame brew flag is 10 s
            // stale during the cycle (polls are deliberately slowed).
            if (machine.z_shot_active || brew_now) {
                s_clean_stage        = CLEAN_RUNNING;
                s_clean_pump_prev    = true;
                s_clean_run_start_ms = millis();
                lv_label_set_text(lbl_clean_overlay_sub, "cycles to go");
                lv_obj_clear_flag(lbl_clean_count, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(obj_clean_stop, LV_OBJ_FLAG_HIDDEN);
            } else if (!machine_clean_active()) {
                // Window expired before the lever was lifted.
                s_clean_stage = CLEAN_IDLE;
                lv_obj_add_flag(obj_clean_overlay, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        case CLEAN_RUNNING: {
            // Time-based countdown; clamped so it never shows 0 while running.
            int left = CLEAN_PHASES - (int)((millis() - s_clean_run_start_ms) / CLEAN_PHASE_MS);
            if (left < 1) left = 1;
            char cbuf[8];
            snprintf(cbuf, sizeof(cbuf), "%d", left);
            lv_label_set_text(lbl_clean_count, cbuf);
            // End of cycle: the machine drops z_shot_active once, at the end.
            bool pump = machine.z_shot_active;
            if ((s_clean_pump_prev && !pump) || !machine_clean_active())
                clean_show_done("Cleaning complete");
            s_clean_pump_prev = pump;
            break;
        }
        default:
            break;
    }

    // Brew-by-weight auto-offset learning: ~6 s after a bbw-stopped shot ends
    // (drips settled), nudge the pre-stop offset by half the target error so
    // the next shot lands closer. Clamped to the offset's settings range.
    static uint32_t s_bbw_settle_ms = 0;
    if (s_bbw_settle_ms != 0 && millis() - s_bbw_settle_ms >= 6000) {
        s_bbw_settle_ms = 0;
        if (scale_connected()) {
            float final_g = scale_weight();
            float err = final_g - settings.brew_target_g;
            if (final_g > 5.0f && fabsf(err) <= 10.0f) {
                float o = settings.prestop_offset_g + 0.5f * err;
                if (o < 0.0f) o = 0.0f;
                if (o > 8.0f) o = 8.0f;
                wlogf("[bbw] auto-offset: final=%.1fg target=%.0fg offset %.1f -> %.1f\n",
                      final_g, settings.brew_target_g, settings.prestop_offset_g, o);
                settings.prestop_offset_g = o;
                settings_save();
            }
        }
    }

    if (!was_brew && brew_now) {
        brew_start_ms  = millis();
        // Clean-cycle pump phases are not shots.
        if (!machine_clean_active()) settings.shot_count++;
        scale_tare_and_start();
        bbw_stop_fired = false;
        // During a cleaning cycle the overlay is the UI — no shot timer.
        if (cur_screen == UI_MAIN && !machine_clean_active()) ui_show_timer();
    }

    // Brew-by-weight auto-stop (gated by BREW_STOP_ENABLED in machine.cpp)
    if (brew_now && cur_screen == UI_TIMER && !bbw_stop_fired) {
        float threshold = settings.brew_target_g - settings.prestop_offset_g;
        if (threshold > 0 && get_display_weight() >= threshold) {
            machine_brew_stop();
            bbw_stop_fired = true;
            Serial.printf("[bbw] threshold %.1fg reached\n", threshold);
        }
    }

    if (was_brew && !brew_now) {
        brew_end_ms    = millis();
        returning_brew = true;
        // Arm auto-offset learning only for shots this firmware stopped.
        if (bbw_stop_fired && scale_connected()) s_bbw_settle_ms = millis();
    }
    if (returning_brew && (millis() - brew_end_ms) >= 3000) {
        returning_brew = false;
        settings_save();
        if (cur_screen == UI_TIMER) ui_show_main();
    }
    was_brew = brew_now;

    if (button_short_press()) {
        switch (cur_screen) {
            case UI_MAIN:
                if (!machine.connected) {
                    if (!demo_brew) { demo_brew = true; demo_brew_start = millis(); }
                    else            { demo_brew = false; }
                }
                break;
            case UI_TIMER:
                break;
            case UI_SETTINGS:
                if (settings_edit) settings_edit_increment();
                else settings_scroll();
                break;
            case UI_WIFI:
            case UI_WIFI_AP:
                break;
        }
    }

    if (button_long_press()) {
        switch (cur_screen) {
            case UI_MAIN:      ui_show_settings(); break;
            case UI_TIMER:     break;
            case UI_SETTINGS:
                if (settings_edit) settings_edit_confirm();
                else settings_select();
                break;
            case UI_WIFI:      ui_show_settings(); break;
            case UI_WIFI_AP:   ESP.restart(); break;
        }
    }

    static uint32_t last_update = 0;
    if (millis() - last_update >= 100) {
        last_update = millis();
        switch (cur_screen) {
            case UI_MAIN:     ui_main_update();  break;
            case UI_TIMER:    ui_timer_update(); break;
            case UI_WIFI:     ui_wifi_update();  break;
            case UI_SCHED:    ui_sched_update(); break;
            case UI_SETTINGS:
            case UI_WIFI_AP:  break;
        }
    }
}

void ui_ota_start() {
    lv_label_set_text(lbl_hint, "OTA updating...");
    lv_timer_handler();
}

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

// Custom 64px Montserrat subset (digits, '.', '-', 'C', degree), generated with
// lv_font_conv for an enlarged lbl_temp. Not wired in yet -- both this and an
// earlier transform_zoom attempt left lbl_temp blank on real hardware; reverted
// to lv_font_montserrat_48 (last confirmed-good) pending proper diagnosis after
// Phase 4 touch bring-up. Defined in src/fonts/lv_font_montserrat_64_temp.c.
LV_FONT_DECLARE(lv_font_montserrat_64_temp);

#define FW_VERSION "v0.15"

// ─── Forward declarations ──────────────────────────────────────────────────────
static void ui_show_main();
static void ui_show_timer();
static void ui_show_settings();
static void ui_show_wifi();
static void ui_show_wifi_ap();
static void settings_draw();
static void settings_edit_increment();
static void main_click_cb(lv_event_t *e);
static void timer_click_cb(lv_event_t *e);
static void settings_row_click_cb(lv_event_t *e);

// ─── Settings items ────────────────────────────────────────────────────────────
enum SettingsItem {
    SI_TEMP,         // 0  88.0–96.0°C, step 0.5
    SI_BREW_WEIGHT,  // 1  20–60 g, step 1 g
    SI_CLEAN,        // 2  action — triggers clean cycle
    SI_PREINFUSION,  // 3  OFF / 0.5–10.0 s, step 0.5
    SI_STANDBY,      // 4  OFF / 15 / 30 / 60 min
    SI_PRESTOP,      // 5  0.0–8.0 g, step 0.5 g
    SI_STEAM,        // 6  ON / OFF
    SI_WIFI,         // 7  navigate → WiFi status screen
    SI_BACK,         // 8  action — save + return to main
    SI_COUNT         // 9
};

static const char *item_names[SI_COUNT] = {
    "Coffee temp",
    "Brew weight",
    "Clean cycle",
    "Pre-infusion",
    "Auto-standby",
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

enum UiScreen { UI_MAIN, UI_TIMER, UI_SETTINGS, UI_WIFI, UI_WIFI_AP };
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
static lv_obj_t *lbl_brew;
static lv_obj_t *lbl_shots;
static lv_obj_t *lbl_scale_weight;
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_hint;

static void ui_main_create() {
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_black(), 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_main, 0, 0);

    lbl_temp = lv_label_create(scr_main);
    lv_label_set_text(lbl_temp, "--.-\xc2\xb0" "C");
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_temp, lv_color_make(0xD4, 0x89, 0x1A), 0);
    lv_obj_align(lbl_temp, LV_ALIGN_LEFT_MID, 8, -12);

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

    lbl_brew = lv_label_create(scr_main);
    lv_label_set_text(lbl_brew, "BREWING");
    lv_obj_set_style_text_font(lbl_brew, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_brew, lv_color_make(0x5C, 0xB8, 0x5C), 0);
    lv_obj_align(lbl_brew, LV_ALIGN_RIGHT_MID, -12, -60);
    lv_obj_add_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);

    lbl_shots = lv_label_create(scr_main);
    lv_label_set_text(lbl_shots, "0 shots");
    lv_obj_set_style_text_font(lbl_shots, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_shots, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_shots, LV_ALIGN_RIGHT_MID, -12, -20);

    lbl_scale_weight = lv_label_create(scr_main);
    lv_label_set_text(lbl_scale_weight, "");
    lv_obj_set_style_text_font(lbl_scale_weight, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_scale_weight, lv_color_make(0x3A, 0x80, 0x3A), 0);
    lv_obj_align(lbl_scale_weight, LV_ALIGN_RIGHT_MID, -12, 24);

    lbl_status = lv_label_create(scr_main);
    lv_label_set_text(lbl_status, "WiFi: connecting...");
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 8, -22);

    // Repurposed as Gicar diagnostic line (always visible, colour-coded)
    lbl_hint = lv_label_create(scr_main);
    lv_label_set_text(lbl_hint, "Gicar: waiting...");
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_hint, lv_color_make(0x70, 0x70, 0x70), 0);
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_LEFT, 8, -6);


    lv_obj_clear_flag(obj_steam,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_steam,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_temp,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_brew,         LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_shots,        LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_scale_weight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_status,       LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(lbl_hint,         LV_OBJ_FLAG_CLICKABLE);
    // lbl_hint is always visible (Gicar diagnostic line)

    lv_obj_add_flag(scr_main, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_main, main_click_cb, LV_EVENT_CLICKED, NULL);
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

    if (get_brew_active())
        lv_obj_clear_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(lbl_brew, LV_OBJ_FLAG_HIDDEN);

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%u shots", (unsigned)settings.shot_count);
    lv_label_set_text(lbl_shots, sbuf);

    if (scale_connected()) {
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "%.1f g", scale_weight());
        lv_label_set_text(lbl_scale_weight, wbuf);
        lv_obj_set_style_text_color(lbl_scale_weight, lv_color_make(0x3A, 0x80, 0x3A), 0);
    } else {
        lv_label_set_text(lbl_scale_weight, "");
    }

    // ── Status line 1: WiFi + MQTT ───────────────────────────────────────────────
    static char stbuf[80];
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    bool mqtt_ok = mqtt_connected();

    if (wifi_ok) {
        const char* mqtt_str = mqtt_ok ? "MQTT:OK" : "MQTT:--";
        if (machine.connected) {
            snprintf(stbuf, sizeof(stbuf), "%s  %s  |  Machine OK",
                     WiFi.localIP().toString().c_str(), mqtt_str);
            lv_obj_set_style_text_color(lbl_status, lv_color_make(0x5C, 0xB8, 0x5C), 0);
        } else {
            snprintf(stbuf, sizeof(stbuf), "%s  %s  RSSI:%d",
                     WiFi.localIP().toString().c_str(), mqtt_str, WiFi.RSSI());
            lv_obj_set_style_text_color(lbl_status, lv_color_make(0xD4, 0x89, 0x1A), 0);
        }
    } else if (WiFi.getMode() == WIFI_OFF) {
        snprintf(stbuf, sizeof(stbuf), "WiFi: off  |  Scale: %s",
                 scale_connected() ? "connected" : "scanning...");
        lv_obj_set_style_text_color(lbl_status, lv_color_make(0x80, 0x80, 0x80), 0);
    } else {
        uint32_t retries = wifi_retry_count();
        if (retries == 0) {
            snprintf(stbuf, sizeof(stbuf), "WiFi: connecting...");
        } else {
            snprintf(stbuf, sizeof(stbuf), "WiFi: retrying (%lu)  MQTT:--", retries);
        }
        lv_obj_set_style_text_color(lbl_status, lv_color_make(0xEF, 0x53, 0x50), 0);
    }
    lv_label_set_text(lbl_status, stbuf);

    // ── Status line 2: Gicar diagnostic + FW version (colour-coded) ────────────
    static char gbuf[64];
    uint32_t rx    = gicar_rx_total();
    uint32_t frms  = gicar_frame_count();
    const char* hs = gicar_handshake_ok() ? "H:OK" : "H:--";
    snprintf(gbuf, sizeof(gbuf), FW_VERSION "  %s  rx:%lu  fr:%lu", hs, rx, frms);
    lv_label_set_text(lbl_hint, gbuf);

    if (frms > 0) {
        lv_obj_set_style_text_color(lbl_hint, lv_color_make(0x5C, 0xB8, 0x5C), 0);  // green — frames OK
    } else if (rx > 0) {
        lv_obj_set_style_text_color(lbl_hint, lv_color_make(0xD4, 0x89, 0x1A), 0);  // amber — bytes but no frames
    } else {
        lv_obj_set_style_text_color(lbl_hint, lv_color_make(0xB0, 0x55, 0x55), 0);  // dark red — no data at all
    }
    lv_obj_clear_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_text_font(lbl_shot_time, &lv_font_montserrat_48, 0);
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
    lv_label_set_text(lbl_target_weight, "target 36g");
    lv_obj_set_style_text_font(lbl_target_weight, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_target_weight, lv_color_make(0x70, 0x70, 0x70), 0);
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

        char targetbuf[20];
        snprintf(targetbuf, sizeof(targetbuf), "target %.0fg", settings.brew_target_g);
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
        case SI_TEMP:
            snprintf(buf, sizeof(buf), "%.1f\xc2\xb0" "C", settings.coffee_temp_c); break;
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
        case SI_WIFI:
            snprintf(buf, sizeof(buf), "%.18s", wifi_config_ssid()); break;
        case SI_BREW_WEIGHT:
            snprintf(buf, sizeof(buf), "%.0f g", settings.brew_target_g); break;
        case SI_PRESTOP:
            snprintf(buf, sizeof(buf), "%.1f g", settings.prestop_offset_g); break;
        case SI_CLEAN:
            snprintf(buf, sizeof(buf), "Run"); break;
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
        bool is_value_item = (i != SI_WIFI && i != SI_CLEAN && i != SI_BACK);
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
        case SI_WIFI:
            ui_show_wifi();
            return;
        case SI_CLEAN:
            machine_trigger_clean();
            lv_label_set_text(lbl_settings_hdr, "Cleaning...");
            {
                time_t now = time(nullptr);
                if (now > 1577836800UL) {
                    settings.last_cleaning_epoch = (uint32_t)now;
                    settings_save();
                }
            }
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
        case SI_TEMP:
            settings.coffee_temp_c += 0.5f;
            if (settings.coffee_temp_c > 96.0f) settings.coffee_temp_c = 88.0f;
            break;
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
        // SI_WIFI, SI_CLEAN, SI_BACK: no increment
    }
    settings_draw();
}

static void settings_edit_confirm() {
    settings_edit = false;
    settings_save();
    if (settings_sel == SI_TEMP)  machine_set_temp(settings.coffee_temp_c);
    if (settings_sel == SI_STEAM) machine_set_steam(settings.steam_on);
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

// TEMP diagnostic: log where taps land on the WiFi screen (screen-level
// CLICKED fires for any tap not consumed by a child).
static void wifi_scr_click_dbg(lv_event_t *e) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    wlogf("[ui] wifi screen tap at x=%d y=%d\n", (int)p.x, (int)p.y);
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

    // TEMP diagnostic: see wifi_scr_click_dbg above.
    lv_obj_add_flag(scr_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_wifi, wifi_scr_click_dbg, LV_EVENT_CLICKED, NULL);

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

// ─── Touch callbacks ───────────────────────────────────────────────────────────

static void main_click_cb(lv_event_t *e) {
    if (cur_screen == UI_MAIN) ui_show_settings();
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
    if (idx == SI_CLEAN) {
        machine_trigger_clean();
        lv_label_set_text(lbl_settings_hdr, "Cleaning...");
        {
            time_t now = time(nullptr);
            if (now > 1577836800UL) {
                settings.last_cleaning_epoch = (uint32_t)now;
                settings_save();
            }
        }
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
    lv_scr_load(scr_main);
}

void ui_tick() {
    demo_update();
    button_update();

    bool brew_now = get_brew_active();

    if (!was_brew && brew_now) {
        brew_start_ms  = millis();
        settings.shot_count++;
        scale_tare_and_start();
        bbw_stop_fired = false;
        if (cur_screen == UI_MAIN) ui_show_timer();
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
            case UI_SETTINGS:
            case UI_WIFI_AP:  break;
        }
    }
}

void ui_ota_start() {
    lv_label_set_text(lbl_status, "OTA updating...");
    lv_timer_handler();
}

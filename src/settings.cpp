#include "settings.h"
#include <Preferences.h>

Settings settings = { 93.5f, 0.0f, false, 0, 36.0f, 3.0f, 0, 0, 0, 50,
                      false, 390, 1200, 120,
                      true };  // clean every 50 shots; schedule off, 06:30/20:00, UTC+2; scale BT on

void settings_init() {
    Preferences prefs;
    prefs.begin("lm", true);  // read-only — safe to call before NVS is written
    settings.coffee_temp_c       = prefs.getFloat("temp",     93.5f);
    settings.preinfusion_s       = prefs.getFloat("preinf",   0.0f);
    settings.steam_on            = prefs.getBool( "steam",    false);
    settings.standby_min         = prefs.getInt(  "standby",  0);
    settings.brew_target_g       = prefs.getFloat("brewwt",   36.0f);
    settings.prestop_offset_g    = prefs.getFloat("prestop",  3.0f);
    settings.shot_count          = prefs.getUInt( "shots",    0);
    settings.last_cleaning_epoch = prefs.getUInt( "clean_ts", 0);
    settings.shots_since_clean   = prefs.getUInt( "shots_cln", 0);
    settings.clean_interval      = prefs.getUShort("clean_int", 50);
    settings.sched_enabled       = prefs.getBool(  "sched_en", false);
    settings.sched_wake_min      = prefs.getUShort("sched_wk", 390);
    settings.sched_sleep_min     = prefs.getUShort("sched_sl", 1200);
    settings.tz_offset_min       = prefs.getShort( "tz_min",   120);
    settings.scale_ble_enabled   = prefs.getBool(  "ble_en",   true);
    prefs.end();
    Serial.printf("[settings] loaded: temp=%.1f preinf=%.1f steam=%d standby=%d "
                  "brewwt=%.0f prestop=%.1f shots=%u clean=%u shots_cln=%u clean_int=%u\n",
                  settings.coffee_temp_c, settings.preinfusion_s, settings.steam_on,
                  settings.standby_min, settings.brew_target_g, settings.prestop_offset_g,
                  settings.shot_count, settings.last_cleaning_epoch, settings.shots_since_clean,
                  settings.clean_interval);
}

void settings_save() {
    Preferences prefs;
    prefs.begin("lm", false);  // read-write
    prefs.putFloat("temp",     settings.coffee_temp_c);
    prefs.putFloat("preinf",   settings.preinfusion_s);
    prefs.putBool( "steam",    settings.steam_on);
    prefs.putInt(  "standby",  settings.standby_min);
    prefs.putFloat("brewwt",   settings.brew_target_g);
    prefs.putFloat("prestop",  settings.prestop_offset_g);
    prefs.putUInt( "shots",    settings.shot_count);
    prefs.putUInt( "clean_ts", settings.last_cleaning_epoch);
    prefs.putUInt( "shots_cln", settings.shots_since_clean);
    prefs.putUShort("clean_int", settings.clean_interval);
    prefs.putBool(  "sched_en", settings.sched_enabled);
    prefs.putUShort("sched_wk", settings.sched_wake_min);
    prefs.putUShort("sched_sl", settings.sched_sleep_min);
    prefs.putShort( "tz_min",   settings.tz_offset_min);
    prefs.putBool(  "ble_en",   settings.scale_ble_enabled);
    prefs.end();
}

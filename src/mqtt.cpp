#include "mqtt.h"
#include "machine.h"
#include "settings.h"
#include "scale.h"
#include "gicar.h"
#include "wlog.h"
#include "ota_http.h"
#include "secrets.h"
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino.h>

// ─── Topic constants ──────────────────────────────────────────────────────────

#define MQTT_BASE       "lm_mini"
#define MQTT_STATE      MQTT_BASE "/state"
#define MQTT_AVAIL      MQTT_BASE "/availability"
#define MQTT_CMD_TEMP   MQTT_BASE "/cmd/temp"
#define MQTT_CMD_STEAM  MQTT_BASE "/cmd/steam"
#define MQTT_CMD_CLEAN  MQTT_BASE "/cmd/clean"
#define MQTT_CMD_STANDBY MQTT_BASE "/cmd/standby"
#define MQTT_CMD_OTA    MQTT_BASE "/cmd/ota"
#define HA_BASE         "homeassistant"

// Default firmware URL used when the ota command payload is "PRESS" (the HA
// button). 192.168.1.62 is the build PC, which serves firmware.bin on :8070.
#define OTA_DEFAULT_URL "http://192.168.1.62:8070/firmware.bin"

// Shared JSON fragments — embedded via compile-time string concatenation
#define AVAIL_J \
    "\"avty_t\":\"" MQTT_AVAIL "\"," \
    "\"pl_avail\":\"online\"," \
    "\"pl_not_avail\":\"offline\""

#define DEV_J \
    "\"device\":{\"ids\":[\"lm_mini_01\"]," \
    "\"name\":\"La Marzocco Mini\"," \
    "\"model\":\"Linea Mini\"," \
    "\"mf\":\"La Marzocco\"}"

// ─── Client ───────────────────────────────────────────────────────────────────

static WiFiClient   s_net;
static PubSubClient s_client(s_net);

// ─── Command handler ──────────────────────────────────────────────────────────

static void on_msg(const char* topic, byte* payload, unsigned int len) {
    // Sized for the longest expected payload: a full firmware URL on cmd/ota
    // (val[32] truncated it to ".../firmwa" — field-debugged 2026-07-09).
    char val[160] = {};
    if (len > sizeof(val) - 1) len = sizeof(val) - 1;
    memcpy(val, payload, len);

    if (strcmp(topic, MQTT_CMD_TEMP) == 0) {
        float t = constrain(atof(val), 88.0f, 96.0f);
        settings.coffee_temp_c = t;
        settings_save();
        machine_set_temp(t);

    } else if (strcmp(topic, MQTT_CMD_STEAM) == 0) {
        settings.steam_on = (strncmp(val, "ON", 2) == 0);
        settings_save();
        machine_set_steam(settings.steam_on);

    } else if (strcmp(topic, MQTT_CMD_STANDBY) == 0) {
        bool go_standby = (strncmp(val, "ON", 2) == 0);
        machine_set_standby(go_standby);

    } else if (strcmp(topic, MQTT_CMD_CLEAN) == 0) {
        machine_clean_start();
        time_t now = time(nullptr);
        // The epoch needs sane (NTP) time; the counter reset must not wait on it.
        if (now > 1577836800UL)    // sanity: after 2020-01-01
            settings.last_cleaning_epoch = (uint32_t)now;
        settings.shots_since_clean = 0;
        settings_save();

    } else if (strcmp(topic, MQTT_CMD_OTA) == 0) {
        // A full URL triggers a pull from that URL; "PRESS" (the HA button) uses
        // the default build-PC URL. Queue it — the update must NOT run inside
        // this callback (long/blocking + reconfigures the radio).
        if (strncmp(val, "http://", 7) == 0) {
            ota_http_request(val);
        } else if (strcmp(val, "PRESS") == 0) {
            ota_http_request(OTA_DEFAULT_URL);
        } else {
            wlogf("[mqtt] ota: ignoring payload '%s'\n", val);
        }
    }
}

// ─── HA discovery ─────────────────────────────────────────────────────────────

static void pub_retained(const char* topic, const char* payload) {
    if (!s_client.publish(topic, payload, true))
        wlogf("[mqtt] publish failed: %s\n", topic);
}

static void publish_discovery() {
    pub_retained(
        HA_BASE "/sensor/lm_mini/coffee_temp/config",
        "{\"name\":\"Coffee Temp\",\"uniq_id\":\"lm_mini_temp\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.temp}}\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/number/lm_mini/target_temp/config",
        "{\"name\":\"Target Temp\",\"uniq_id\":\"lm_mini_target_temp\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.target_temp}}\","
        "\"cmd_t\":\"" MQTT_CMD_TEMP "\","
        "\"min\":88,\"max\":96,\"step\":0.5,\"unit_of_meas\":\"\\u00b0C\","
        "\"mode\":\"slider\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/binary_sensor/lm_mini/brewing/config",
        "{\"name\":\"Brewing\",\"uniq_id\":\"lm_mini_brew\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.brew}}\","
        "\"dev_cla\":\"running\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/switch/lm_mini/steam/config",
        "{\"name\":\"Steam\",\"uniq_id\":\"lm_mini_steam\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.steam}}\","
        "\"cmd_t\":\"" MQTT_CMD_STEAM "\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/switch/lm_mini/standby/config",
        "{\"name\":\"Standby\",\"uniq_id\":\"lm_mini_standby\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.standby}}\","
        "\"cmd_t\":\"" MQTT_BASE "/cmd/standby\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/sensor/lm_mini/shot_count/config",
        "{\"name\":\"Shot Count\",\"uniq_id\":\"lm_mini_shots\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.shots}}\","
        "\"state_class\":\"total_increasing\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/sensor/lm_mini/shots_since_clean/config",
        "{\"name\":\"Shots since clean\",\"uniq_id\":\"lm_mini_shots_since_clean\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.shots_since_clean}}\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/sensor/lm_mini/last_cleaning/config",
        "{\"name\":\"Last Cleaning\",\"uniq_id\":\"lm_mini_last_clean\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.last_clean}}\","
        "\"dev_cla\":\"timestamp\"," AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/binary_sensor/lm_mini/machine_online/config",
        "{\"name\":\"Machine Online\",\"uniq_id\":\"lm_mini_machine\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.machine}}\","
        "\"dev_cla\":\"connectivity\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        AVAIL_J "," DEV_J "}");

    pub_retained(
        HA_BASE "/button/lm_mini/clean/config",
        "{\"name\":\"Start Cleaning\",\"uniq_id\":\"lm_mini_clean\","
        "\"cmd_t\":\"" MQTT_CMD_CLEAN "\",\"pl_prs\":\"PRESS\","
        AVAIL_J "," DEV_J "}");

    // The machine's OWN setpoint register (0x0007), re-read every 60 s —
    // distinct from Target Temp, which is only our stored/commanded value.
    pub_retained(
        HA_BASE "/sensor/lm_mini/machine_setpoint/config",
        "{\"name\":\"Machine Setpoint\",\"uniq_id\":\"lm_mini_machine_setpoint\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.machine_setpoint}}\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Firmware OTA trigger — press publishes "PRESS" to the ota command topic,
    // which pulls firmware.bin from the build PC (see OTA_DEFAULT_URL).
    pub_retained(
        HA_BASE "/button/lm_mini/update_fw/config",
        "{\"name\":\"Update firmware\",\"uniq_id\":\"lm_mini_update_fw\","
        "\"cmd_t\":\"" MQTT_CMD_OTA "\",\"pl_prs\":\"PRESS\","
        AVAIL_J "," DEV_J "}");

    wlogf("[mqtt] HA discovery published (12 entities)\n");
}

// ─── State publish ────────────────────────────────────────────────────────────

static void publish_state() {
    char clean_str[28];
    if (settings.last_cleaning_epoch > 0) {
        time_t t = (time_t)settings.last_cleaning_epoch;
        struct tm tm_buf;
        gmtime_r(&t, &tm_buf);
        strftime(clean_str, sizeof(clean_str), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    } else {
        strncpy(clean_str, "unknown", sizeof(clean_str));
    }

    char dbg[32];
    gicar_debug_info(dbg, sizeof(dbg));

    bool in_frame; int rxlen;
    gicar_rx_state(&in_frame, &rxlen);

    char state[416];
    snprintf(state, sizeof(state),
        "{\"temp\":%.1f,\"target_temp\":%.1f,\"machine_setpoint\":%.1f,\"brew\":\"%s\","
        "\"steam\":\"%s\",\"standby\":\"%s\",\"shots\":%u,\"shots_since_clean\":%u,\"last_clean\":\"%s\","
        "\"machine\":\"%s\",\"scale\":\"%s\",\"weight\":%.1f,"
        "\"rx\":%lu,\"in_frame\":%d,\"rxlen\":%d,\"dbg\":\"%s\"}",
        machine.coffee_temp_c,
        settings.coffee_temp_c,
        machine.setpoint_c,
        machine.brew_active   ? "ON" : "OFF",
        machine.steam_active  ? "ON" : "OFF",
        machine.standby       ? "ON" : "OFF",
        (unsigned)settings.shot_count,
        (unsigned)settings.shots_since_clean,
        clean_str,
        machine.connected    ? "ON" : "OFF",
        scale_connected()    ? "ON" : "OFF",
        scale_weight(),
        gicar_rx_total(),
        in_frame ? 1 : 0,
        rxlen,
        dbg);

    if (!s_client.publish(MQTT_STATE, state))
        wlogf("[mqtt] publish failed — buffer full or disconnected\n");
}

// ─── Connect ──────────────────────────────────────────────────────────────────

static bool do_connect() {
    s_net.setTimeout(1);  // limit TCP connect to 1 s — prevents 3 s freezes on broker drop
    char client_id[20];
    snprintf(client_id, sizeof(client_id), "lm_mini_%04X",
             (unsigned)(ESP.getEfuseMac() & 0xFFFF));

    bool ok = (strlen(MQTT_USER) > 0)
        ? s_client.connect(client_id, MQTT_USER, MQTT_PASS,
                           MQTT_AVAIL, 0, true, "offline")
        : s_client.connect(client_id, nullptr, nullptr,
                           MQTT_AVAIL, 0, true, "offline");

    if (!ok) {
        wlogf("[mqtt] connect failed rc=%d\n", s_client.state());
        return false;
    }

    s_client.publish(MQTT_AVAIL, "online", true);
    s_client.subscribe(MQTT_CMD_TEMP);
    s_client.subscribe(MQTT_CMD_STEAM);
    s_client.subscribe(MQTT_CMD_STANDBY);
    s_client.subscribe(MQTT_CMD_CLEAN);
    s_client.subscribe(MQTT_CMD_OTA);
    publish_discovery();
    wlogf("[mqtt] connected as %s\n", client_id);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void mqtt_init() {
    s_client.setServer(MQTT_HOST, MQTT_PORT);
    s_client.setCallback(on_msg);
    s_client.setBufferSize(512);
}

void mqtt_tick() {
    // If WiFi dropped, force-disconnect MQTT so the stale TCP socket is cleared
    // and the reconnect logic below can re-establish cleanly.
    if (WiFi.status() != WL_CONNECTED) {
        if (s_client.connected()) {
            s_client.disconnect();
            wlogf("[mqtt] WiFi down — disconnected\n");
        }
        return;
    }

    if (!s_client.connected()) {
        // Never block the loop during an active brew — any TCP connect delay
        // would freeze the shot timer display.
        if (machine.brew_active) return;
        static uint32_t s_last_attempt = 0;
        if (millis() - s_last_attempt >= 5000) {
            s_last_attempt = millis();
            do_connect();
        }
        return;
    }

    s_client.loop();

    static uint32_t s_last_state = 0;
    if (millis() - s_last_state >= 2000) {
        s_last_state = millis();
        publish_state();
    }
}

bool mqtt_connected() {
    return s_client.connected();
}

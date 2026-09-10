#include "mqtt.h"
#include "machine.h"
#include "settings.h"
#include "scale.h"
#include "gicar.h"
#include "wlog.h"
#include "ota_http.h"
#include "mqtt_config.h"
#include "flow_log.h"
#include "bbw_event.h"
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino.h>
#include <lwip/sockets.h>
// lwip/sockets.h defines connect/write/read as macros, which would mangle
// s_client.connect(...) below. The Arduino core's own NetworkClient.cpp undefines
// them for exactly this reason — mirror it.
#undef connect
#undef write
#undef read

// ─── Topic constants ──────────────────────────────────────────────────────────

#define MQTT_BASE       "lm_mini"
#define MQTT_STATE      MQTT_BASE "/state"
#define MQTT_AVAIL      MQTT_BASE "/availability"
#define MQTT_SHOT       MQTT_BASE "/shot"
#define MQTT_BBW        MQTT_BASE "/bbw"
#define MQTT_CMD_TEMP   MQTT_BASE "/cmd/temp"
#define MQTT_CMD_STEAM  MQTT_BASE "/cmd/steam"
#define MQTT_CMD_CLEAN  MQTT_BASE "/cmd/clean"
#define MQTT_CMD_STANDBY MQTT_BASE "/cmd/standby"
#define MQTT_CMD_OTA    MQTT_BASE "/cmd/ota"
#define MQTT_CMD_PRESTOP MQTT_BASE "/cmd/prestop"
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

// ─── Blocking-write guard ─────────────────────────────────────────────────────

// "Would a write to the broker socket block right now?"
//
// Every write ultimately lands in NetworkClient::write(), whose behaviour when
// the TCP send buffer is full is hard-coded and NOT overridable: it retries a
// 1 s select() up to WIFI_CLIENT_MAX_WRITE_RETRY (10) times, i.e. it can park the
// single-threaded main loop for ten seconds on one publish. On a lossy WiFi day
// that starves machine_update(), which then declares the GICAR link dead after
// 3 s of no frames and blanks the temperature display (field-proven 2026-09-08).
//
// So instead of writing and hoping, probe first with a zero-timeout select() on
// the same fd: if the socket is not writable we simply skip this tick. The probe
// itself never blocks.
static bool net_writable() {
    int fd = s_net.fd();
    if (fd < 0) return false;          // no socket at all (never connected / stopped)

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = {0, 0};        // poll, do not wait
    if (select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) return false;
    return FD_ISSET(fd, &wset) != 0;
}

// Millis at which the socket was first seen unwritable in the current stall, or 0
// while it is writable. A stall this long means the peer has stopped acking and
// the socket will never drain — see MQTT_STALL_DROP_MS.
static uint32_t s_stall_since_ms = 0;

// How long the socket may stay unwritable before we tear it down and rebuild it.
// Comfortably longer than a transient WiFi retry burst, far shorter than the TCP
// stack's own (minutes-long) retransmit give-up.
#define MQTT_STALL_DROP_MS 15000

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

    } else if (strcmp(topic, MQTT_CMD_PRESTOP) == 0) {
        float g = constrain(atof(val), 0.0f, 8.0f);
        settings.prestop_offset_g = g;
        settings_save();
        wlogf("[mqtt] prestop offset set to %.1f g\n", g);

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

// Returns the publish result so the incremental driver below can retry the same
// entity on a later tick instead of losing its config message.
static bool pub_retained(const char* topic, const char* payload) {
    bool ok = s_client.publish(topic, payload, true);
    if (!ok) wlogf("[mqtt] publish failed: %s\n", topic);
    return ok;
}

// Number of discovery writes: 20 entity configs plus the one legacy-entity delete
// (case 12). The "(20 entities)" log below counts entities, not writes.
#define DISCOVERY_COUNT 21

// One discovery write per call, selected by index. Split out of the old
// publish_discovery(), which sent all 21 in a single pass — 21 back-to-back
// retained publishes on a degraded link was a 20-100 s loop stall. Payloads are
// unchanged; only the dispatch around them is new.
static bool publish_discovery_entity(int i) {
    switch (i) {
    case 0: return pub_retained(
        HA_BASE "/sensor/lm_mini/coffee_temp/config",
        "{\"name\":\"Coffee Temp\",\"uniq_id\":\"lm_mini_temp\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.temp}}\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    case 1: return pub_retained(
        HA_BASE "/number/lm_mini/target_temp/config",
        "{\"name\":\"Target Temp\",\"uniq_id\":\"lm_mini_target_temp\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.target_temp}}\","
        "\"cmd_t\":\"" MQTT_CMD_TEMP "\","
        "\"min\":88,\"max\":96,\"step\":0.5,\"unit_of_meas\":\"\\u00b0C\","
        "\"mode\":\"slider\"," AVAIL_J "," DEV_J "}");

    case 2: return pub_retained(
        HA_BASE "/binary_sensor/lm_mini/brewing/config",
        "{\"name\":\"Brewing\",\"uniq_id\":\"lm_mini_brew\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.brew}}\","
        "\"dev_cla\":\"running\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        AVAIL_J "," DEV_J "}");

    case 3: return pub_retained(
        HA_BASE "/switch/lm_mini/steam/config",
        "{\"name\":\"Steam\",\"uniq_id\":\"lm_mini_steam\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.steam}}\","
        "\"cmd_t\":\"" MQTT_CMD_STEAM "\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," AVAIL_J "," DEV_J "}");

    case 4: return pub_retained(
        HA_BASE "/switch/lm_mini/standby/config",
        "{\"name\":\"Standby\",\"uniq_id\":\"lm_mini_standby\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.standby}}\","
        "\"cmd_t\":\"" MQTT_BASE "/cmd/standby\","
        "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"," AVAIL_J "," DEV_J "}");

    case 5: return pub_retained(
        HA_BASE "/sensor/lm_mini/shot_count/config",
        "{\"name\":\"Shot Count\",\"uniq_id\":\"lm_mini_shots\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.shots}}\","
        "\"state_class\":\"total_increasing\"," AVAIL_J "," DEV_J "}");

    case 6: return pub_retained(
        HA_BASE "/sensor/lm_mini/shots_since_clean/config",
        "{\"name\":\"Shots since clean\",\"uniq_id\":\"lm_mini_shots_since_clean\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.shots_since_clean}}\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    case 7: return pub_retained(
        HA_BASE "/sensor/lm_mini/last_cleaning/config",
        "{\"name\":\"Last Cleaning\",\"uniq_id\":\"lm_mini_last_clean\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.last_clean}}\","
        "\"dev_cla\":\"timestamp\"," AVAIL_J "," DEV_J "}");

    case 8: return pub_retained(
        HA_BASE "/binary_sensor/lm_mini/machine_online/config",
        "{\"name\":\"Machine Online\",\"uniq_id\":\"lm_mini_machine\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.machine}}\","
        "\"dev_cla\":\"connectivity\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        AVAIL_J "," DEV_J "}");

    case 9: return pub_retained(
        HA_BASE "/button/lm_mini/clean/config",
        "{\"name\":\"Start Cleaning\",\"uniq_id\":\"lm_mini_clean\","
        "\"cmd_t\":\"" MQTT_CMD_CLEAN "\",\"pl_prs\":\"PRESS\","
        AVAIL_J "," DEV_J "}");

    // The machine's OWN setpoint register (0x0007), re-read every 60 s —
    // distinct from Target Temp, which is only our stored/commanded value.
    case 10: return pub_retained(
        HA_BASE "/sensor/lm_mini/machine_setpoint/config",
        "{\"name\":\"Machine Setpoint\",\"uniq_id\":\"lm_mini_machine_setpoint\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.machine_setpoint}}\","
        "\"dev_cla\":\"temperature\",\"unit_of_meas\":\"\\u00b0C\","
        "\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Brew-by-weight pre-stop offset — the machine stops the shot this many grams
    // before target to allow for post-stop drip. Settable from HA (cmd/prestop) so
    // a post-shot automation can tune it against the measured final weight.
    case 11: return pub_retained(
        HA_BASE "/number/lm_mini/prestop_offset/config",
        "{\"name\":\"Pre-stop offset\",\"uniq_id\":\"lm_mini_prestop_offset\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.prestop_offset}}\","
        "\"cmd_t\":\"" MQTT_CMD_PRESTOP "\","
        "\"min\":0,\"max\":8,\"step\":0.1,\"unit_of_meas\":\"g\","
        "\"mode\":\"box\"," AVAIL_J "," DEV_J "}");

    // Delete the stale prestop_offset *sensor* entity from before it became a
    // number: an empty retained payload on the old discovery topic tells HA to
    // remove it. pub_retained can't send an empty payload (see below), so publish
    // directly.
    case 12: {
        const char* topic = HA_BASE "/sensor/lm_mini/prestop_offset/config";
        bool ok = s_client.publish(topic, "", true);
        if (!ok) wlogf("[mqtt] publish failed: %s\n", topic);
        return ok;
    }

    // Live scale reading — the current cup weight during a shot.
    case 13: return pub_retained(
        HA_BASE "/sensor/lm_mini/weight/config",
        "{\"name\":\"Scale Weight\",\"uniq_id\":\"lm_mini_weight\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.weight}}\","
        "\"unit_of_meas\":\"g\",\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Whether a BLE scale is currently linked.
    case 14: return pub_retained(
        HA_BASE "/binary_sensor/lm_mini/scale/config",
        "{\"name\":\"Scale Connected\",\"uniq_id\":\"lm_mini_scale\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.scale}}\","
        "\"dev_cla\":\"connectivity\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\","
        AVAIL_J "," DEV_J "}");

    // Brew-by-weight target — the final cup weight the shot aims for. Exposed so a
    // post-shot automation can compare it against the measured weight.
    case 15: return pub_retained(
        HA_BASE "/sensor/lm_mini/brew_target/config",
        "{\"name\":\"Brew Target\",\"uniq_id\":\"lm_mini_brew_target\","
        "\"stat_t\":\"" MQTT_STATE "\",\"val_tpl\":\"{{value_json.brew_target}}\","
        "\"unit_of_meas\":\"g\",\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Firmware OTA trigger — press publishes "PRESS" to the ota command topic,
    // which pulls firmware.bin from the build PC (see OTA_DEFAULT_URL).
    case 16: return pub_retained(
        HA_BASE "/button/lm_mini/update_fw/config",
        "{\"name\":\"Update firmware\",\"uniq_id\":\"lm_mini_update_fw\","
        "\"cmd_t\":\"" MQTT_CMD_OTA "\",\"pl_prs\":\"PRESS\","
        AVAIL_J "," DEV_J "}");

    // ── Per-shot flow curve (retained on MQTT_SHOT, one publish per finished shot) ──
    //
    // These three read from MQTT_SHOT rather than MQTT_STATE, so they only change
    // at brew end and hold their last value in between.

    // Fastest instantaneous flow seen during the shot.
    case 17: return pub_retained(
        HA_BASE "/sensor/lm_mini/peak_flow/config",
        "{\"name\":\"Peak Flow\",\"uniq_id\":\"lm_mini_peak_flow\","
        "\"stat_t\":\"" MQTT_SHOT "\",\"val_tpl\":\"{{value_json.peak_flow_gps}}\","
        "\"unit_of_meas\":\"g/s\",\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Final cup weight divided by shot duration.
    case 18: return pub_retained(
        HA_BASE "/sensor/lm_mini/avg_flow/config",
        "{\"name\":\"Average Flow\",\"uniq_id\":\"lm_mini_avg_flow\","
        "\"stat_t\":\"" MQTT_SHOT "\",\"val_tpl\":\"{{value_json.avg_flow_gps}}\","
        "\"unit_of_meas\":\"g/s\",\"state_class\":\"measurement\"," AVAIL_J "," DEV_J "}");

    // Carrier for the full curve: the state is the shot duration, while the t/f/w
    // arrays ride along as entity attributes (json_attr_t) for charting in HA.
    case 19: return pub_retained(
        HA_BASE "/sensor/lm_mini/shot_curve/config",
        "{\"name\":\"Shot Flow Curve\",\"uniq_id\":\"lm_mini_shot_curve\","
        "\"stat_t\":\"" MQTT_SHOT "\",\"val_tpl\":\"{{value_json.duration_ms}}\","
        "\"unit_of_meas\":\"ms\",\"json_attr_t\":\"" MQTT_SHOT "\"," AVAIL_J "," DEV_J "}");

    // ── Per-shot brew-by-weight diagnostic record (retained on MQTT_BBW) ────────
    // State is the stop reason so the history reads as a sequence of outcomes;
    // everything needed to diagnose a runaway or an early stop (attempts, stop
    // latency, arm timing, weights) rides along as entity attributes.
    case 20: return pub_retained(
        HA_BASE "/sensor/lm_mini/bbw_last_shot/config",
        "{\"name\":\"BBW Last Shot\",\"uniq_id\":\"lm_mini_bbw_last_shot\","
        "\"stat_t\":\"" MQTT_BBW "\",\"val_tpl\":\"{{value_json.stop_reason}}\","
        "\"json_attr_t\":\"" MQTT_BBW "\"," AVAIL_J "," DEV_J "}");

    default: return true;   // unreachable: the driver never exceeds DISCOVERY_COUNT
    }
}

// Incremental discovery driver state. s_disc_next is the index still to send;
// s_disc_done latches once all of them have gone out. Discovery is retained on
// the broker, so a later reconnect within the same boot must NOT resend it — the
// configs are already there, and re-sending them is exactly the burst we removed.
static int  s_disc_next = 0;
static bool s_disc_done = false;

// Send at most one discovery message. Caller guarantees connected + writable +
// not brewing. A failed publish leaves the index alone so the same entity is
// retried on the next tick.
static void discovery_tick() {
    if (s_disc_done) return;
    if (!publish_discovery_entity(s_disc_next)) return;
    if (++s_disc_next >= DISCOVERY_COUNT) {
        s_disc_done = true;
        wlogf("[mqtt] HA discovery published (20 entities)\n");
    }
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

    char state[512];
    snprintf(state, sizeof(state),
        "{\"temp\":%.1f,\"target_temp\":%.1f,\"machine_setpoint\":%.1f,\"brew\":\"%s\","
        "\"steam\":\"%s\",\"standby\":\"%s\",\"shots\":%u,\"shots_since_clean\":%u,\"last_clean\":\"%s\","
        "\"machine\":\"%s\",\"scale\":\"%s\",\"weight\":%.1f,\"prestop_offset\":%.1f,\"brew_target\":%.1f,"
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
        settings.prestop_offset_g,
        settings.brew_target_g,
        gicar_rx_total(),
        in_frame ? 1 : 0,
        rxlen,
        dbg);

    if (!s_client.publish(MQTT_STATE, state))
        wlogf("[mqtt] publish failed — buffer full or disconnected\n");
}

// ─── Shot-flow publish ────────────────────────────────────────────────────────

// Push a finished shot's flow curve, retained, so HA still has the last shot
// after a restart. No-op unless flow_log has a curve waiting, so this is safe to
// call on every periodic tick.
static void publish_shot_flow() {
    if (!flow_log_has_pending()) return;
    String json = flow_log_take_json();
    if (!s_client.publish(MQTT_SHOT, json.c_str(), true)) {
        wlogf("[mqtt] shot-flow publish failed (%u bytes)\n", (unsigned)json.length());
    }
}

// ─── Brew-by-weight event publish ─────────────────────────────────────────────

// Push the last shot's bbw diagnostic record, retained, so HA keeps the most
// recent outcome across restarts. Same pending-flag handshake (and the same
// no-op-when-empty safety) as publish_shot_flow().
static void publish_bbw_event() {
    if (!bbw_event_has_pending()) return;
    String json = bbw_event_take_json();
    if (!s_client.publish(MQTT_BBW, json.c_str(), true)) {
        wlogf("[mqtt] bbw-event publish failed (%u bytes)\n", (unsigned)json.length());
    }
}

// ─── Connect ──────────────────────────────────────────────────────────────────

static bool do_connect() {
    // Cap the TCP connect at 1 s. The core's default is
    // WIFI_CLIENT_DEF_CONN_TIMEOUT_MS (3000), long enough to starve
    // machine_update() past its 3 s GICAR timeout and blank the display on every
    // failed attempt. NOTE: this is setConnectionTimeout(), not setTimeout() —
    // the latter is the Stream *read* timeout and had no effect on connect
    // (v0.48 and earlier called it in the belief it did).
    s_net.setConnectionTimeout(1000);
    char client_id[20];
    snprintf(client_id, sizeof(client_id), "lm_mini_%04X",
             (unsigned)(ESP.getEfuseMac() & 0xFFFF));

    bool ok = (strlen(mqtt_config_user()) > 0)
        ? s_client.connect(client_id, mqtt_config_user(), mqtt_config_pass(),
                           MQTT_AVAIL, 0, true, "offline")
        : s_client.connect(client_id, nullptr, nullptr,
                           MQTT_AVAIL, 0, true, "offline");

    if (!ok) return false;   // caller logs, and owns the retry backoff

    // A freshly connected socket is writable in practice, but gate it anyway so
    // there is exactly one rule for "never write blind".
    if (net_writable()) s_client.publish(MQTT_AVAIL, "online", true);
    s_client.subscribe(MQTT_CMD_TEMP);
    s_client.subscribe(MQTT_CMD_STEAM);
    s_client.subscribe(MQTT_CMD_STANDBY);
    s_client.subscribe(MQTT_CMD_CLEAN);
    s_client.subscribe(MQTT_CMD_OTA);
    s_client.subscribe(MQTT_CMD_PRESTOP);
    // Discovery is NOT sent here any more: 21 retained publishes in one pass was
    // the single worst loop stall in the module. mqtt_tick() now drips them out
    // one per tick (see discovery_tick).
    wlogf("[mqtt] connected as %s\n", client_id);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void mqtt_init() {
    // NVS-first broker config (survives CI-built OTA updates with blank secrets).
    mqtt_config_init();
    if (!mqtt_config_enabled()) return;   // no broker — mqtt_tick() no-ops
    s_client.setServer(mqtt_config_host(), mqtt_config_port());
    s_client.setCallback(on_msg);
    // Must cover topic + header + the largest payload, since PubSubClient drops
    // (not truncates) oversized publishes. The worst case is no longer the ~500
    // byte state JSON but the per-shot flow curve on MQTT_SHOT: 150 samples of
    // t/f/w serialised as parallel arrays runs to roughly 3 KB.
    s_client.setBufferSize(4096);
    // Cap PubSubClient's blocking socket reads at 1 s. Its default 15 s CONNACK/
    // read timeout can freeze loop() (and the shot timer) if the broker stalls.
    s_client.setSocketTimeout(1);
}

void mqtt_tick() {
    // No broker configured (e.g. a CI-stubbed build on a fresh device): stay
    // fully idle rather than spin trying to connect to an empty host.
    if (!mqtt_config_enabled()) return;

    // If WiFi dropped, force-disconnect MQTT so the stale TCP socket is cleared
    // and the reconnect logic below can re-establish cleanly.
    if (WiFi.status() != WL_CONNECTED) {
        if (s_client.connected()) {
            s_client.disconnect();
            wlogf("[mqtt] WiFi down — disconnected\n");
        }
        return;
    }

    // Reconnect, with exponential backoff. A fixed 5 s retry meant that on a bad
    // WiFi day the loop paid a (capped, but still real) connect attempt every 5 s
    // for as long as the broker was unreachable; backing off to 60 s keeps the
    // recovery latency acceptable while making the stall rare.
    static uint32_t s_last_attempt  = 0;
    static uint32_t s_retry_ms      = 5000;   // wait before the NEXT attempt
    static uint8_t  s_fails         = 0;      // consecutive failures, drives the ladder
    if (!s_client.connected()) {
        s_stall_since_ms = 0;   // not a write stall — there is no socket to stall
        // Never block the loop during an active brew — any TCP connect delay
        // would freeze the shot timer display.
        if (machine.brew_active) return;
        if (millis() - s_last_attempt >= s_retry_ms) {
            s_last_attempt = millis();
            if (do_connect()) {
                s_fails    = 0;
                s_retry_ms = 5000;   // recovered — back to the fast retry
            } else {
                // 5, 10, 20, 40, 60, 60 ... s. Compute the new interval BEFORE
                // logging it, so the logged number is the one the >= above will
                // actually use next time round.
                if (s_fails < 5) s_fails++;
                s_retry_ms = (s_fails >= 5) ? 60000u : (5000u << (s_fails - 1));
                wlogf("[mqtt] connect failed rc=%d — next try in %u s\n",
                      s_client.state(), (unsigned)(s_retry_ms / 1000));
            }
        }
        return;
    }

    // One probe per tick drives every write decision below. If the socket cannot
    // take a byte right now, we do nothing at all this tick — including
    // s_client.loop(), whose keep-alive PINGREQ is itself a write.
    bool writable = net_writable();
    if (!writable) {
        if (s_stall_since_ms == 0) s_stall_since_ms = millis();
        if (millis() - s_stall_since_ms >= MQTT_STALL_DROP_MS) {
            // The peer has stopped acking; this socket will not drain. Tear it
            // down so the reconnect path above builds a fresh one.
            //
            // Order matters: s_client.disconnect() writes a 2-byte DISCONNECT
            // packet, which on a socket we have just proven unwritable would enter
            // the very 10 s retry loop this whole guard exists to avoid. Close the
            // fd first — with fd() < 0 the write returns immediately and
            // disconnect() still clears PubSubClient's state flags.
            wlogf("[mqtt] socket stalled 15 s — dropping connection\n");
            s_net.stop();
            s_client.disconnect();
            s_stall_since_ms = 0;
        }
        return;
    }
    s_stall_since_ms = 0;

    s_client.loop();   // cheap keep-alive; safe to run even mid-shot

    // Suspend periodic publishes during an active shot: publish_state() serialises
    // a ~450-byte frame and can block on a slow broker, jittering the shot timer.
    // s_client.loop() above keeps the connection alive; we just skip the writes.
    // Publish once immediately after the shot ends to catch the state back up.
    static bool     s_was_brewing = false;
    static uint32_t s_last_state  = 0;
    if (machine.brew_active) {
        s_was_brewing = true;
        return;
    }
    // Not brewing, connected and writable: this is the only place discovery is
    // sent, one config per tick, so it can never become a burst again.
    discovery_tick();

    if (s_was_brewing) {
        s_was_brewing = false;
        s_last_state  = millis();
        publish_state();   // immediate catch-up after the shot
        return;
    }

    if (millis() - s_last_state >= 2000) {
        s_last_state = millis();
        publish_state();
        // Deliberately not in the falling-edge branch above: the ~3 KB curve is a
        // blocking write, and brew end is exactly the instant shot_log.cpp defers
        // work away from (the UI is animating its return to Main). Publishing on
        // the periodic tick also means a curve recorded while the broker was down
        // survives — mqtt_tick() returns early while disconnected, so the pending
        // flag holds and the curve goes out on the first tick after reconnect. A
        // publish that fails on a live connection does drop that shot (logged).
        // Re-probe between writes: publish_state() may itself have filled the send
        // buffer, and the curve is by far the largest write in the module.
        if (net_writable()) publish_shot_flow();
        if (net_writable()) publish_bbw_event();
    }
}

bool mqtt_connected() {
    return s_client.connected();
}

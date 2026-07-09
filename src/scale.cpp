#include "scale.h"
#include "machine.h"
#include "wlog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>

// ─── BLE identifiers ─────────────────────────────────────────────────────────

// Felicita Arc
// !! UNVERIFIED — UUIDs from community sources. Confirm with nRF Connect before
// !! trusting. Weight format: (byte[1]<<8|byte[2])/10.0 g, negative if byte[3]==1.
#define FELICITA_NAME_PREFIX  "FELICITA"
#define FELICITA_SVC_UUID     "0000FFE0-0000-1000-8000-00805F9B34FB"
#define FELICITA_CHAR_UUID    "0000FFE1-0000-1000-8000-00805F9B34FB"

// Bookoo Themis Ultra — source: github.com/BooKooCode/OpenSource, July 2025
// Checksum = XOR of all preceding bytes in the packet (Header1^Header2^Data...).
// NOTE: some checksum values in the BooKoo docs table do not match the stated
// XOR formula. The formula is confirmed correct by tare (0x08) and calibrate
// (0x00) and is used here rather than the table's potentially-erroneous bytes.
#define BOOKOO_NAME_PREFIX    "Bookoo"
#define BOOKOO_NAME_PREFIX2   "BOOKOO"
#define BOOKOO_SVC_UUID       "00000FFE-0000-1000-8000-00805F9B34FB"
#define BOOKOO_NOTIFY_UUID    "0000FF11-0000-1000-8000-00805F9B34FB"
#define BOOKOO_WRITE_UUID     "0000FF12-0000-1000-8000-00805F9B34FB"

// Command DATA1 bytes (DATA2 and DATA3 are 0x00 for all basic commands)
#define BOOKOO_CMD_TARE          0x01
#define BOOKOO_CMD_START         0x04
#define BOOKOO_CMD_STOP          0x05
#define BOOKOO_CMD_RESET         0x06
#define BOOKOO_CMD_TARE_AND_START 0x07

// ─── Shared state ─────────────────────────────────────────────────────────────

ScaleState scale = { SCALE_NONE, false, 0.0f, 0.0f, 0, 0, true };

static NimBLEAddress s_target_addr;
static ScaleModel    s_target_model = SCALE_NONE;
static bool          s_found = false;
static volatile bool s_ota_hold = false;  // set from the OTA task (see scale.h)
// True only while the scale task is parked at its gate (no NimBLE calls in
// flight). scale_radio_release() waits for this before NimBLEDevice::deinit(),
// making the cross-task deinit race-free.
static volatile bool s_ble_parked = false;

// Bookoo write characteristic — only valid while BLE task holds an active connection.
// Commands from other tasks are queued via s_bookoo_pending_cmd instead of calling
// writeValue directly, keeping all NimBLE calls on the BLE task (core 0).
static NimBLERemoteCharacteristic* s_bookoo_write_char = nullptr;
static volatile uint8_t            s_bookoo_pending_cmd = 0;  // 0 = none

// ─── Bookoo helpers ───────────────────────────────────────────────────────────

static uint8_t bookoo_checksum(const uint8_t* buf, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) cs ^= buf[i];
    return cs;
}

static void bookoo_send_cmd(uint8_t data1, uint8_t data2, uint8_t data3) {
    if (!s_bookoo_write_char) return;
    uint8_t pkt[6] = { 0x03, 0x0A, data1, data2, data3, 0 };
    pkt[5] = bookoo_checksum(pkt, 5);
    s_bookoo_write_char->writeValue(pkt, 6, false);
}

// ─── Bookoo notify callback (called from NimBLE task, core 0) ─────────────────

static void bookoo_notify_cb(NimBLERemoteCharacteristic* pChar,
                             uint8_t* pData, size_t len, bool isNotify) {
    if (len < 20) return;

    // Verify packet type
    if (pData[0] != 0x03 || pData[1] != 0x0B) return;

    // Verify checksum (XOR of bytes 0–18 must equal byte 19)
    if (bookoo_checksum(pData, 19) != pData[19]) return;

    // Timer: bytes [2..4] big-endian, milliseconds
    uint32_t ms = ((uint32_t)pData[2] << 16) | ((uint32_t)pData[3] << 8) | pData[4];

    // Weight: byte [6] = sign ('+'/'-'), bytes [7..9] = weight*100 big-endian
    uint32_t w_raw = ((uint32_t)pData[7] << 16) | ((uint32_t)pData[8] << 8) | pData[9];
    float w = w_raw / 100.0f;
    if (pData[6] == '-') w = -w;

    // Flow rate: byte [10] = sign, bytes [11..12] = flow*100 big-endian
    uint16_t f_raw = ((uint16_t)pData[11] << 8) | pData[12];
    float f = f_raw / 100.0f;
    if (pData[10] == '-') f = -f;

    uint8_t batt = pData[13];

    scale.weight_g    = w;
    scale.flow_gps    = f;
    scale.timer_ms    = ms;
    scale.battery_pct = batt;
    scale.stable      = true;
}

static bool bookoo_subscribe(NimBLEClient* pClient) {
    NimBLERemoteService* pSvc = pClient->getService(BOOKOO_SVC_UUID);
    if (!pSvc) {
        wlogf("[scale] Bookoo: service 0FFE not found\n");
        return false;
    }

    NimBLERemoteCharacteristic* pNotify = pSvc->getCharacteristic(BOOKOO_NOTIFY_UUID);
    if (!pNotify || !pNotify->canNotify()) {
        wlogf("[scale] Bookoo: notify char FF11 missing or not notifiable\n");
        return false;
    }

    NimBLERemoteCharacteristic* pWrite = pSvc->getCharacteristic(BOOKOO_WRITE_UUID);
    if (!pWrite || !pWrite->canWrite()) {
        wlogf("[scale] Bookoo: write char FF12 missing or not writable\n");
        return false;
    }

    pNotify->subscribe(true, bookoo_notify_cb);
    s_bookoo_write_char = pWrite;
    wlogf("[scale] Bookoo Themis Ultra: subscribed\n");
    return true;
}

// ─── Felicita Arc notify callback ─────────────────────────────────────────────

static void felicita_notify_cb(NimBLERemoteCharacteristic* pChar,
                               uint8_t* pData, size_t len, bool isNotify) {
    if (len < 3) return;
    uint16_t raw = ((uint16_t)pData[1] << 8) | pData[2];
    float w = raw / 10.0f;
    if (len >= 4 && pData[3] == 1) w = -w;  // negative reading
    scale.weight_g = w;
    scale.stable   = (len >= 5) ? ((pData[4] & 0x02) == 0) : true;
}

static bool felicita_subscribe(NimBLEClient* pClient) {
    NimBLERemoteService* pSvc = pClient->getService(FELICITA_SVC_UUID);
    if (!pSvc) {
        Serial.println("[scale] Felicita: service FFE0 not found — verify UUID with nRF Connect");
        return false;
    }
    NimBLERemoteCharacteristic* pChar = pSvc->getCharacteristic(FELICITA_CHAR_UUID);
    if (!pChar || !pChar->canNotify()) {
        Serial.println("[scale] Felicita: char FFE1 missing or not notifiable");
        return false;
    }
    pChar->subscribe(true, felicita_notify_cb);
    Serial.println("[scale] Felicita Arc: subscribed to weight notifications");
    return true;
}

// ─── BLE client callbacks ─────────────────────────────────────────────────────

// Logs WHY the link dropped: reason 0x08 (BLE_ERR_CONN_SPVN_TMO) = supervision
// timeout, i.e. WiFi/BLE coex starved the connection on this single-radio chip;
// 0x13 (REM_USER_CONN_TERM) = the scale itself hung up.
class ScaleClientCb : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient* pClient, int reason) override {
        wlogf("[scale] link dropped, reason=0x%02X\n", reason);
    }
};

// ─── BLE scan callback ────────────────────────────────────────────────────────

// File-static single instance (not new'd per boot/resume). setScanCallbacks() is
// registered with deleteCallbacks=false so NimBLEDevice::deinit() never tries to
// free this static — a resume re-registers the same instance. A new'd callback
// would leak one object per suspend/resume cycle.
class ScaleScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (s_found) return;
        std::string name = dev->getName();
        if (name.empty()) return;

        ScaleModel m = SCALE_NONE;
        if (name.rfind(FELICITA_NAME_PREFIX, 0) == 0) {
            m = SCALE_FELICITA_ARC;
        } else if (name.rfind(BOOKOO_NAME_PREFIX, 0) == 0 ||
                   name.rfind(BOOKOO_NAME_PREFIX2, 0) == 0) {
            m = SCALE_BOOKOO_THEMIS;
        }

        if (m != SCALE_NONE) {
            s_found       = true;
            s_target_addr = dev->getAddress();
            s_target_model = m;
            NimBLEDevice::getScan()->stop();
            wlogf("[scale] found: %s\n", name.c_str());
        }
    }
};

static ScaleScanCb s_scan_cb;

// ─── BLE setup helper ─────────────────────────────────────────────────────────

// One-time-per-radio-lifecycle BLE bring-up: init the stack and configure the
// scan. Factored out of scale_ble_task() so scale_radio_resume() can bring BLE
// back with the IDENTICAL configuration after a suspend/deinit. Does NOT wait
// for WiFi (the caller owns that) and does NOT start a scan (the task loop does).
static void scale_ble_setup() {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);

    // NimBLE 2.x's start() is already non-blocking (its 2nd arg is isContinue,
    // not a blocking flag -- verified against the installed NimBLE-Arduino
    // headers). The stalling we measured on real hardware (temp display
    // flickering every ~5s, 9s+ delay before the shot timer appears) comes
    // from the BLE controller/host's own background radio activity during
    // the scan window competing with loop() for the single CPU core -- not
    // from our task blocking. Mitigate by cutting scan duty cycle hard
    // (interval/window was 120/60 = 50%; now 320/16 = 5%) so the radio
    // spends far less time actively scanning per unit time.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    // deleteCallbacks=false: s_scan_cb is a file-static, never freed by deinit().
    pScan->setScanCallbacks(&s_scan_cb, false);
    // Active scan is REQUIRED: the Bookoo Themis advertises its name only in
    // the scan response, which passive scanning never requests — with
    // setActiveScan(false) the scale is invisible to the name filter
    // (confirmed 2026-07-03: Pi-side active scan saw "BOOKOO_SC_U 624216"
    // while the firmware's passive scan never logged a find). Passive mode
    // was a mitigation for loop stalls whose real cause was blocking serial
    // writes (fixed in main.cpp/wlog.cpp), not scan radio contention.
    pScan->setActiveScan(true);
    // 50% duty (75 ms interval / 37.5 ms window, the reference bring-up's
    // values). The 320/16 = 5% duty tried here earlier made discovery fail
    // outright — three consecutive 10 s scans missed a Bookoo the Pi's
    // adapter saw instantly. Like passive mode above, the low duty was a
    // mitigation for the disproven radio-starvation theory; scans never run
    // during a brew, so aggressive scanning costs nothing that matters.
    pScan->setInterval(120);
    pScan->setWindow(60);
}

// ─── BLE task ─────────────────────────────────────────────────────────────────

static void scale_ble_task(void*) {
    // Wait for WiFi before scanning — shared 2.4GHz radio; BLE scanning during
    // association prevents WiFi from completing in the machine's noisy environment.
    // Skip when WiFi is intentionally disabled (WIFI_ENABLED 0).
    if (WiFi.getMode() != WIFI_OFF) {
        while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
        vTaskDelay(pdMS_TO_TICKS(2000));  // brief settle after connect
    }

    scale_ble_setup();

    for (;;) {
        // Don't scan during an active brew — BLE scan disrupts the shared WiFi
        // radio and can corrupt R-frame responses. The scale isn't needed for
        // reconnection mid-shot; wait until the shot ends. Same hold applies
        // during an OTA transfer (see scale_set_ota_hold / scale_radio_release).
        // Publish s_ble_parked while waiting so scale_radio_release() can safely
        // deinit BLE from another task: no NimBLE calls run on this task here.
        if (machine.brew_active || s_ota_hold) {
            s_ble_parked = true;
            while (machine.brew_active || s_ota_hold) vTaskDelay(pdMS_TO_TICKS(500));
            s_ble_parked = false;
        }

        // Fetch the scan handle AFTER the park block: a scale_radio_suspend()/
        // resume() cycle deinit(true)s and re-inits NimBLE while we're parked,
        // invalidating any handle held across the park. getScan() returns the
        // current (post-resume) singleton, so this is always live here.
        NimBLEScan* pScan = NimBLEDevice::getScan();

        s_found       = false;
        s_target_model = SCALE_NONE;
        pScan->clearResults();

        wlogf("[scale] scanning (10s)...\n");
        // NimBLE 2.x start() takes MILLISECONDS (1.x took seconds) and is
        // NON-blocking — it returns immediately. Poll s_found for the scan's
        // 10 s duration; onResult() stops the scan early on a match. Without
        // this wait, s_found was checked microseconds after the scan began,
        // so the connect path below never executed.
        pScan->start(10000, true);
        // Also break on s_ota_hold: without it, an OTA radio-release that lands
        // mid-scan would wait out the full ~10s here before the task could park,
        // overrunning scale_radio_release()'s deinit wait (race → crash).
        for (int i = 0; i < 100 && !s_found && !s_ota_hold; i++) vTaskDelay(pdMS_TO_TICKS(100));
        pScan->stop();

        // OTA release requested during the scan window — park now (gate at top).
        if (s_ota_hold) continue;

        if (!s_found) {
            // 30s pause between scans when no scale found — BLE scan thrashes the
            // shared WiFi radio; short pauses cause MQTT dropouts.
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        // ── Connect ──────────────────────────────────────────────────────────
        NimBLEClient* pClient = NimBLEDevice::createClient(s_target_addr);
        // NimBLE 2.x: milliseconds (1.x took seconds) — setConnectTimeout(5)
        // was a 5 ms timeout, so every connect attempt failed instantly.
        pClient->setConnectTimeout(5000);
        pClient->setClientCallbacks(new ScaleClientCb(), true);
        // Relaxed connection params for WiFi/BLE coex on the shared radio:
        // 30–60 ms interval, slave latency 4, 4 s supervision timeout. The
        // NimBLE defaults (12ms/0/...) leave no scheduling slack when WiFi is
        // active and the first live link died ~6 s after subscribing.
        pClient->setConnectionParams(24, 48, 4, 400);

        if (!pClient->connect()) {
            wlogf("[scale] connect failed, retrying...\n");
            NimBLEDevice::deleteClient(pClient);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // ── Subscribe ────────────────────────────────────────────────────────
        bool ok = false;
        if (s_target_model == SCALE_FELICITA_ARC) {
            ok = felicita_subscribe(pClient);
        } else if (s_target_model == SCALE_BOOKOO_THEMIS) {
            ok = bookoo_subscribe(pClient);
        }

        if (!ok) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        scale.model     = s_target_model;
        scale.connected = true;
        wlogf("[scale] %s connected\n", scale_model_name());

        // ── Stay connected ───────────────────────────────────────────────────
        // Break on s_ota_hold so an OTA radio release can proceed: the loop exits,
        // the client is cleaned up below, and the task parks at the gate.
        while (pClient->isConnected() && !s_ota_hold) {
            // Drain pending command queue (Bookoo only)
            if (s_target_model == SCALE_BOOKOO_THEMIS) {
                uint8_t cmd = s_bookoo_pending_cmd;
                if (cmd != 0) {
                    s_bookoo_pending_cmd = 0;
                    bookoo_send_cmd(cmd, 0, 0);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        scale.connected   = false;
        scale.weight_g    = 0.0f;
        scale.flow_gps    = 0.0f;
        scale.timer_ms    = 0;
        scale.battery_pct = 0;
        scale.model       = SCALE_NONE;
        s_bookoo_write_char   = nullptr;
        s_bookoo_pending_cmd  = 0;
        wlogf("[scale] disconnected — rescanning in 3s\n");
        NimBLEDevice::deleteClient(pClient);
        // Short pause: an awake scale sleeps within minutes, so a 30 s wait
        // here often meant never reconnecting at all. (The 30 s pause between
        // *unsuccessful* scans above still protects WiFi at idle.)
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void scale_init() {
    // Priority 0 (below the Arduino loop task's default priority 1) -- on this
    // single-core C6, this favors loop()'s time-critical GICAR bit-banged UART
    // polling and LVGL rendering whenever both are runnable simultaneously.
    xTaskCreate(scale_ble_task, "Scale", 8192, NULL, 0, NULL);
}

void scale_tare() {
    if (!scale.connected) return;
    if (scale.model == SCALE_BOOKOO_THEMIS) {
        s_bookoo_pending_cmd = BOOKOO_CMD_TARE;
    } else {
        Serial.println("[scale] tare: Felicita command sequence not yet documented");
    }
}

void scale_tare_and_start() {
    if (!scale.connected) return;
    if (scale.model == SCALE_BOOKOO_THEMIS) {
        s_bookoo_pending_cmd = BOOKOO_CMD_TARE_AND_START;
    }
}

bool scale_connected() {
    return scale.connected;
}

float scale_weight() {
    return scale.weight_g;
}

const char* scale_model_name() {
    switch (scale.model) {
        case SCALE_FELICITA_ARC:    return "Felicita Arc";
        case SCALE_BOOKOO_THEMIS:   return "Bookoo Themis Ultra";
        default:                    return "—";
    }
}

void scale_set_ota_hold(bool hold) {
    s_ota_hold = hold;
    // A 10s scan may already be in flight — kill it now rather than letting
    // it fight the OTA stream for the radio. Safe if BLE isn't initialized
    // yet (task not started / pre-WiFi): guard on init state.
    if (hold && NimBLEDevice::isInitialized()) {
        NimBLEDevice::getScan()->stop();
    }
}

void scale_radio_suspend() {
    wlogf("[scale] radio suspend — parking task, deinit BLE\n");

    // 1) Raise the hold so the scale task exits any scan/connect loop and heads
    //    for its park gate; 2) kill any in-flight scan immediately.
    s_ota_hold = true;
    if (NimBLEDevice::isInitialized()) {
        NimBLEDevice::getScan()->stop();
    }

    // 3) Wait for the task to reach the gate (s_ble_parked). With the hold
    //    checks on every loop, the only blocking NimBLE call left is a ~5s
    //    connect() (plus its 5s fail backoff), so 8s is a safe upper bound: by
    //    then the task holds no NimBLE calls even if s_ble_parked never flipped.
    for (int i = 0; i < 80 && !s_ble_parked; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 4) Fully free the radio for WiFi. Pausing the scan is not enough on the
    //    S3's single shared 2.4GHz radio; deinit(true) also clears NimBLE state
    //    so WiFi.setSleep(false) becomes legal. deinit STRICTLY AFTER the task
    //    parked (step 3) — no NimBLE call is in flight, so this is race-free.
    if (NimBLEDevice::isInitialized()) {
        NimBLEDevice::deinit(true);
    }
    wlogf("[scale] BLE deinitialized (parked=%d)\n", s_ble_parked ? 1 : 0);
}

void scale_radio_resume() {
    wlogf("[scale] radio resume — re-init BLE, unpark task\n");

    // Re-init the stack with the identical scan configuration BEFORE clearing the
    // hold. The parked task spins in its gate on s_ota_hold and makes NO NimBLE
    // calls until the hold drops; re-initializing first guarantees the task never
    // touches a torn-down stack when it unparks (race-free counterpart to the
    // deinit-after-park ordering in scale_radio_suspend()).
    if (!NimBLEDevice::isInitialized()) {
        scale_ble_setup();
    }

    // Drop the hold last: the task now leaves its gate, re-fetches the fresh scan
    // handle, and resumes scanning — reconnecting to the scale naturally.
    s_ota_hold = false;
}

// Reboot-ending OTA paths (install / ArduinoOTA) release and never resume.
void scale_radio_release() {
    scale_radio_suspend();
}

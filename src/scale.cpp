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
        Serial.println("[scale] Bookoo: service 0FFE not found");
        return false;
    }

    NimBLERemoteCharacteristic* pNotify = pSvc->getCharacteristic(BOOKOO_NOTIFY_UUID);
    if (!pNotify || !pNotify->canNotify()) {
        Serial.println("[scale] Bookoo: notify char FF11 missing or not notifiable");
        return false;
    }

    NimBLERemoteCharacteristic* pWrite = pSvc->getCharacteristic(BOOKOO_WRITE_UUID);
    if (!pWrite || !pWrite->canWrite()) {
        Serial.println("[scale] Bookoo: write char FF12 missing or not writable");
        return false;
    }

    pNotify->subscribe(true, bookoo_notify_cb);
    s_bookoo_write_char = pWrite;
    Serial.println("[scale] Bookoo Themis Ultra: subscribed");
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

// ─── BLE scan callback ────────────────────────────────────────────────────────

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

// ─── BLE task ─────────────────────────────────────────────────────────────────

static void scale_ble_task(void*) {
    // Wait for WiFi before scanning — shared 2.4GHz radio; BLE scanning during
    // association prevents WiFi from completing in the machine's noisy environment.
    // Skip when WiFi is intentionally disabled (WIFI_ENABLED 0).
    if (WiFi.getMode() != WIFI_OFF) {
        while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
        vTaskDelay(pdMS_TO_TICKS(2000));  // brief settle after connect
    }

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
    pScan->setScanCallbacks(new ScaleScanCb(), false);
    pScan->setActiveScan(false);
    pScan->setInterval(320);
    pScan->setWindow(16);

    for (;;) {
        // Don't scan during an active brew — BLE scan disrupts the shared WiFi
        // radio and can corrupt R-frame responses. The scale isn't needed for
        // reconnection mid-shot; wait until the shot ends.
        while (machine.brew_active) vTaskDelay(pdMS_TO_TICKS(500));

        s_found       = false;
        s_target_model = SCALE_NONE;
        pScan->clearResults();

        wlogf("[scale] scanning (10s)...\n");
        pScan->start(10, true);   // blocking 10-second scan

        if (!s_found) {
            // 30s pause between scans when no scale found — BLE scan thrashes the
            // shared WiFi radio; short pauses cause MQTT dropouts.
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        // ── Connect ──────────────────────────────────────────────────────────
        NimBLEClient* pClient = NimBLEDevice::createClient(s_target_addr);
        pClient->setConnectTimeout(5);

        if (!pClient->connect()) {
            Serial.println("[scale] connect failed, retrying...");
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
        while (pClient->isConnected()) {
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
        wlogf("[scale] disconnected — waiting 30s before rescan\n");
        NimBLEDevice::deleteClient(pClient);
        vTaskDelay(pdMS_TO_TICKS(30000));  // give WiFi radio 30s clear before BLE rescan
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

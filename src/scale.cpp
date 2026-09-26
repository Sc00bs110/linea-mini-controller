#include "scale.h"
#include "machine.h"
#include "settings.h"
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
//
// REQUIRED scale mode: FLOW mode (two white LEDs) with AUTO OFF (Themis Ultra
// manual + field tests 2026-09-26). In Flow mode these commands act like the left
// button: 0x07 tares and starts the clock, 0x05 stops and HOLDS the time, 0x06
// clears it. Flow mode has no automatic tare on cup placement; our 0x07 at every
// brew start replaces the right-button tare. In Auto mode the scale's own state
// machine shows a flashing brew summary after timing stops that only a physical
// button tap clears (no BLE command does), and STOP zeroes the reported clock so
// the shot time cannot be held. 0x04 (START) never started the clock on this
// scale; 0x07 does, so 0x04 is not used.
#define BOOKOO_CMD_TARE          0x01
#define BOOKOO_CMD_START         0x04
#define BOOKOO_CMD_STOP          0x05
#define BOOKOO_CMD_RESET         0x06
#define BOOKOO_CMD_TARE_AND_START 0x07

// ─── Shared state ─────────────────────────────────────────────────────────────

ScaleState scale = { SCALE_NONE, false, 0.0f, 0.0f, 0, 0, true, 0 };

static NimBLEAddress s_target_addr;
static ScaleModel    s_target_model = SCALE_NONE;
static bool          s_found = false;

// Scan-cadence state (see the no-scale pause in scale_ble_task): scan fast until
// the first-ever connect, and for a window after any disconnect, so an in-use
// scale reconnects quickly; relax afterwards to protect WiFi coexistence.
static bool          s_scale_ever_connected = false;
static uint32_t      s_last_disconnect_ms   = 0;    // millis() of last link drop (0 = none)
static const uint32_t SCAN_FAST_MS          = 2000;  // no-scale pause while a scale is expected
static const uint32_t SCAN_RELAXED_MS       = 60000; // no-scale pause once idle (WiFi protection)
// 60 s: each scan itself runs 10 s, so a 15 s pause meant ~40% BLE radio duty at
// idle — enough to degrade WiFi/MQTT within minutes (observed 2026-07-15).
static const uint32_t RECONNECT_FAST_WIN_MS = 60000; // fast-scan window after a disconnect
static volatile bool s_ota_hold = false;  // set from the OTA task (see scale.h)
// Set by scale_kick_fast_scan() (UI pill, loop core), consumed by the scale
// task's inter-scan pause loop so a manual reconnect cuts the relaxed 60 s wait
// short and re-scans within one ~500 ms slice. Volatile: cross-task flag.
static volatile bool s_scan_kick = false;
// True only while the scale task is parked at its gate (no NimBLE calls in
// flight). scale_radio_release() waits for this before NimBLEDevice::deinit(),
// making the cross-task deinit race-free.
static volatile bool s_ble_parked = false;
// Link phase for the UI's SCALE pill (see ScaleLink in scale.h). Written ONLY
// by scale_ble_task so there is a single writer; the UI just polls it. IDLE
// until the task first reaches its gate (it waits for WiFi before that).
static volatile ScaleLink s_link = SCALE_LINK_IDLE;

// Bookoo write characteristic — only valid while BLE task holds an active connection.
// Commands from other tasks are queued via the command ring below instead of
// calling writeValue directly, keeping all NimBLE calls on the BLE task.
static NimBLERemoteCharacteristic* s_bookoo_write_char = nullptr;

// ─── Bookoo command ring ──────────────────────────────────────────────────────
//
// v0.51 replaces the old single-slot s_bookoo_pending_cmd: a shot start now needs
// RESET followed by TARE_AND_START, and a flush needs STOP followed by RESET, so a
// one-slot mailbox would overwrite the first command before the BLE task sent it.
//
// Lock-free single-producer / single-consumer ring:
//   producer = Arduino loop task (ui.cpp and the mqtt.cpp callbacks), and ONLY
//              the producer writes s_cmd_tail;
//   consumer = scale_ble_task, and ONLY the consumer writes s_cmd_head.
// Indices are free-running uint8_t counters (slot = index & MASK); 256 is a
// multiple of the ring size, so (uint8_t)(tail - head) is the fill level across
// wrap-around. The Scale task is created unpinned, so on the S3 it may run on the
// other core from loop(): volatile alone does not order stores between cores,
// hence the explicit __sync_synchronize() fences at each publish/consume point.
//
// Staleness: every entry carries the connection epoch it was queued under and
// its enqueue time. The consumer discards an entry from an earlier connection or
// older than CMD_MAX_AGE_MS instead of sending it — a clock command that arrives
// seconds late (or on a fresh link) would act on the wrong shot. The ring is never
// cleared from the consumer side (that would race the producer's tail).
struct BookooCmd {
    uint8_t  cmd;       // BOOKOO_CMD_* DATA1 byte
    uint8_t  epoch;     // s_epoch at enqueue time
    uint32_t enq_ms;    // millis() at enqueue time
};
static const uint8_t  CMD_RING_SIZE  = 8;               // must divide 256
static const uint8_t  CMD_RING_MASK  = CMD_RING_SIZE - 1;
static const uint32_t CMD_MAX_AGE_MS = 1500;            // older entries are discarded
static const uint32_t CMD_GAP_MS     = 100;             // min spacing between writes
static BookooCmd        s_cmd_ring[CMD_RING_SIZE];
static volatile uint8_t s_cmd_head = 0;   // next entry to send   (consumer-owned)
static volatile uint8_t s_cmd_tail = 0;   // next free slot       (producer-owned)
// Connection epoch. Incremented ONLY by scale_ble_task: before scale.connected
// goes true on connect, and where it goes false on disconnect. A producer that
// sees scale.connected and then stamps the epoch can therefore only ever stamp
// the current link's epoch or a stale one — never a future link's.
static volatile uint8_t s_epoch = 0;

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

static const char* bookoo_cmd_name(uint8_t cmd) {
    switch (cmd) {
        case BOOKOO_CMD_TARE:           return "TARE";
        case BOOKOO_CMD_START:          return "START";
        case BOOKOO_CMD_STOP:           return "STOP";
        case BOOKOO_CMD_RESET:          return "RESET";
        case BOOKOO_CMD_TARE_AND_START: return "TARE_AND_START";
        default:                        return "?";
    }
}

// Producer side (Arduino loop task only). Queues n commands as ONE unit: all
// entries are written first and the tail is advanced once, so the consumer sees
// either none or all of them. If fewer than n slots are free the whole push is
// dropped — a lone TARE_AND_START without its preceding RESET, or a lone RESET
// without its STOP, would leave the clock in a state the caller did not ask for.
static bool bookoo_push(const uint8_t* cmds, uint8_t n) {
    // Gate on a live Bookoo link: entries are stamped with the current epoch, and
    // queuing while disconnected would only be discarded later anyway.
    if (!scale.connected || scale.model != SCALE_BOOKOO_THEMIS) return false;

    uint8_t tail = s_cmd_tail;                        // our own index
    uint8_t used = (uint8_t)(tail - s_cmd_head);      // cast: wrap-safe fill level
    if ((uint8_t)(CMD_RING_SIZE - used) < n) {
        wlogf("[scale] cmd ring full (%u queued), dropped %u cmd(s) starting %s\n",
              (unsigned)used, (unsigned)n, bookoo_cmd_name(cmds[0]));
        return false;
    }

    uint8_t  ep  = s_epoch;
    uint32_t now = millis();
    for (uint8_t i = 0; i < n; i++) {
        BookooCmd& e = s_cmd_ring[(uint8_t)(tail + i) & CMD_RING_MASK];
        e.cmd    = cmds[i];
        e.epoch  = ep;
        e.enq_ms = now;
    }
    __sync_synchronize();          // entries visible before the tail that publishes them
    s_cmd_tail = (uint8_t)(tail + n);
    return true;
}

static bool bookoo_push1(uint8_t cmd) {
    return bookoo_push(&cmd, 1);
}

// Deferred "what did the clock do?" reports, BLE task only. Kept as low-volume
// field diagnostics: each send is followed by a timer_ms readback 600 ms later
// (and 1200 ms for the start commands, so a clock that is not advancing — e.g. a
// scale left in the wrong mode — is visible). Several slots because RESET and
// TARE_AND_START go out ~100 ms apart and each needs its line.
struct CmdReport {
    uint8_t  cmd;        // 0 = slot free
    uint8_t  stage;      // 0 = +600 pending, 1 = +1200 pending
    uint32_t sent_ms;
};
static const uint8_t CMD_REPORT_SLOTS = 4;
static CmdReport     s_cmd_reports[CMD_REPORT_SLOTS];

static void cmd_report_add(uint8_t cmd, uint32_t sent_ms) {
    // Take a free slot; if all are busy, overwrite the oldest (diagnostic only).
    uint8_t pick = 0;
    for (uint8_t i = 0; i < CMD_REPORT_SLOTS; i++) {
        if (s_cmd_reports[i].cmd == 0) { pick = i; break; }
        if (s_cmd_reports[i].sent_ms - s_cmd_reports[pick].sent_ms > 0x80000000UL)
            pick = i;    // i was sent earlier than pick (wrap-safe comparison)
    }
    s_cmd_reports[pick].cmd     = cmd;
    s_cmd_reports[pick].stage   = 0;
    s_cmd_reports[pick].sent_ms = sent_ms;
}

static void cmd_reports_tick() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < CMD_REPORT_SLOTS; i++) {
        CmdReport& r = s_cmd_reports[i];
        if (r.cmd == 0) continue;
        uint32_t due = (r.stage == 0) ? 600 : 1200;
        if (now - r.sent_ms < due) continue;
        wlogf("[scale] timer_ms now %lu (%s+%lums)\n", (unsigned long)scale.timer_ms,
              bookoo_cmd_name(r.cmd), (unsigned long)due);
        bool is_start = (r.cmd == BOOKOO_CMD_START || r.cmd == BOOKOO_CMD_TARE_AND_START);
        if (r.stage == 0 && is_start) r.stage = 1;
        else                          r.cmd   = 0;
    }
}

static void cmd_reports_clear() {
    for (uint8_t i = 0; i < CMD_REPORT_SLOTS; i++) s_cmd_reports[i].cmd = 0;
}

// Consumer side (scale_ble_task only). Discards any stale entries at the head,
// then sends at most ONE live command, and only if CMD_GAP_MS has passed since the
// previous write (back-to-back unacknowledged writes risk the scale dropping one).
// Returns true while entries remain queued so the caller can poll faster.
static bool bookoo_drain(uint32_t& last_write_ms) {
    for (;;) {
        uint8_t head = s_cmd_head;                    // our own index
        if (head == s_cmd_tail) return false;         // empty
        __sync_synchronize();                         // tail seen before reading its entries
        BookooCmd e = s_cmd_ring[head & CMD_RING_MASK];
        uint32_t now = millis();

        bool stale = (e.epoch != s_epoch) || (now - e.enq_ms > CMD_MAX_AGE_MS) ||
                     s_target_model != SCALE_BOOKOO_THEMIS;
        if (!stale && now - last_write_ms < CMD_GAP_MS)
            return true;                              // live, but too soon — next pass

        __sync_synchronize();                         // finish reading before freeing the slot
        s_cmd_head = (uint8_t)(head + 1);

        if (stale) {
            wlogf("[scale] cmd %s discarded (stale: age=%lums epoch=%u/%u)\n",
                  bookoo_cmd_name(e.cmd), (unsigned long)(now - e.enq_ms),
                  (unsigned)e.epoch, (unsigned)s_epoch);
            continue;                                 // look at the next entry
        }

        bookoo_send_cmd(e.cmd, 0, 0);
        last_write_ms = millis();
        wlogf("[scale] cmd %s sent (timer_ms=%lu)\n", bookoo_cmd_name(e.cmd),
              (unsigned long)scale.timer_ms);
        cmd_report_add(e.cmd, last_write_ms);
        return s_cmd_head != s_cmd_tail;
    }
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

    scale.weight_g       = w;
    scale.flow_gps       = f;
    scale.timer_ms       = ms;
    scale.battery_pct    = batt;
    scale.stable         = true;
    scale.last_weight_ms = millis();  // timestamp for the UI bbw staleness failsafe
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
    scale.weight_g       = w;
    scale.stable         = (len >= 5) ? ((pData[4] & 0x02) == 0) : true;
    scale.last_weight_ms = millis();  // timestamp for the UI bbw staleness failsafe
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
        // Also park while the machine is in standby (only when not already
        // connected to a scale): nobody brews on a sleeping machine, and
        // continuous scanning degrades WiFi/MQTT overnight (observed
        // 2026-07-14: OTA triggers lost to MQTT reconnect churn).
        // Also park when the user has disabled the scale radio (settings toggle):
        // no scanning at all until it is switched back on. A scale that was
        // connected when the toggle went OFF is dropped by the "Stay connected"
        // loop below (which also gates on settings.scale_ble_enabled), so the
        // task arrives here already disconnected and simply waits.
        // The link state is refreshed every slice so the UI can tell "BT off"
        // (OFF) from any other park reason (PAUSED) even if the reason changes
        // while parked. This also covers scale_radio_suspend(): its hold parks
        // the task here, so the link reads PAUSED once the task reaches the gate
        // (it may first sit out a post-failure/disconnect delay reading IDLE).
        if (machine.brew_active || s_ota_hold || machine.standby ||
            !settings.scale_ble_enabled) {
            s_ble_parked = true;
            while (machine.brew_active || s_ota_hold || machine.standby ||
                   !settings.scale_ble_enabled) {
                s_link = settings.scale_ble_enabled ? SCALE_LINK_PAUSED : SCALE_LINK_OFF;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            s_ble_parked = false;
            s_link = SCALE_LINK_IDLE;
        }

        // Fetch the scan handle AFTER the park block: a scale_radio_suspend()/
        // resume() cycle deinit(true)s and re-inits NimBLE while we're parked,
        // invalidating any handle held across the park. getScan() returns the
        // current (post-resume) singleton, so this is always live here.
        NimBLEScan* pScan = NimBLEDevice::getScan();

        s_found       = false;
        s_target_model = SCALE_NONE;
        pScan->clearResults();

        s_link = SCALE_LINK_SCANNING;
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
            // Scan ended without a match: the UI reads SCANNING -> IDLE as a
            // failed search (red "NO SCALE" after a tap).
            s_link = SCALE_LINK_IDLE;
            // BLE scanning thrashes the shared WiFi radio, so pauses between scans
            // protect MQTT/OTA. But scan FAST (2 s) until the first-ever connect,
            // and for RECONNECT_FAST_WIN_MS after a disconnect, so an in-use scale
            // is (re)found quickly — an awake scale sleeps within minutes, and a
            // long cadence often meant never catching it. Relax to 15 s once a
            // scale has been gone a while (idle machine, no scale around).
            // Fast from boot until the first-ever connect, but time-bounded so a
            // scaleless machine doesn't scan at ~83% duty forever (which would
            // thrash the shared radio the relaxed cadence exists to protect); and
            // fast for a window after any disconnect for quick reconnection.
            // Sliced pause (500 ms slices) rather than one blocking vTaskDelay so
            // a UI reconnect kick (scale_kick_fast_scan) takes effect within ~1 s
            // even during the 60 s relaxed wait — a single long delay could not be
            // interrupted. Recompute the target each slice so an expiring fast
            // window naturally lengthens the remaining wait. Also break promptly if
            // a park condition appears so the gate at the top of the loop is honored.
            uint32_t pause_start = millis();
            for (;;) {
                if (s_scan_kick) { s_scan_kick = false; break; }
                if (machine.brew_active || s_ota_hold || machine.standby ||
                    !settings.scale_ble_enabled)
                    break;
                bool since_boot_fast = !s_scale_ever_connected &&
                                       millis() < RECONNECT_FAST_WIN_MS;
                bool since_drop_fast = s_last_disconnect_ms != 0 &&
                                       millis() - s_last_disconnect_ms < RECONNECT_FAST_WIN_MS;
                bool fast = since_boot_fast || since_drop_fast;
                if (millis() - pause_start >= (fast ? SCAN_FAST_MS : SCAN_RELAXED_MS))
                    break;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
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

        s_link = SCALE_LINK_CONNECTING;
        if (!pClient->connect()) {
            s_link = SCALE_LINK_IDLE;   // UI treats this as a failed search
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
            s_link = SCALE_LINK_IDLE;   // found but unusable: also a failed search
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        scale.model            = s_target_model;
        // New epoch BEFORE connected goes true: any command the loop task queues
        // once it sees this link is stamped with this epoch, and anything left
        // in the ring from an earlier link is recognisably stale.
        s_epoch                = (uint8_t)(s_epoch + 1);
        scale.connected        = true;
        s_link                 = SCALE_LINK_CONNECTED;
        s_scale_ever_connected = true;  // gates scan cadence (see no-scale pause above)
        wlogf("[scale] %s connected\n", scale_model_name());

        // ── Stay connected ───────────────────────────────────────────────────
        // Break on s_ota_hold so an OTA radio release can proceed: the loop exits,
        // the client is cleaned up below, and the task parks at the gate.
        // Break on !settings.scale_ble_enabled too: turning the toggle OFF while
        // connected drops the link here (cleanup below issues a clean disconnect
        // via deleteClient), then the task parks at the gate above.
        // Drain the command ring: one command per pass, >= CMD_GAP_MS apart. Poll
        // every 50 ms while commands are queued (a RESET+TARE_AND_START pair lands
        // ~100 ms apart instead of 400 ms), 200 ms otherwise as before.
        uint32_t last_write_ms = millis() - CMD_GAP_MS;
        while (pClient->isConnected() && !s_ota_hold && settings.scale_ble_enabled) {
            bool pending = bookoo_drain(last_write_ms);
            cmd_reports_tick();
            vTaskDelay(pdMS_TO_TICKS(pending ? 50 : 200));
        }

        // Pending readbacks belong to the dead link; a "timer_ms now 0" line
        // after the drop would read like a command effect.
        cmd_reports_clear();
        // Epoch moves with connected=false: entries queued on this link are
        // discarded, never sent on the next one. The ring itself is NOT cleared
        // here — only the producer may touch the tail.
        s_epoch              = (uint8_t)(s_epoch + 1);
        scale.connected      = false;
        s_link               = SCALE_LINK_IDLE;
        scale.weight_g       = 0.0f;
        scale.flow_gps       = 0.0f;
        scale.timer_ms       = 0;
        scale.battery_pct    = 0;
        scale.model          = SCALE_NONE;
        scale.last_weight_ms = 0;            // no live feed → bbw failsafe sees "never"
        s_last_disconnect_ms = millis();     // fast-rescan window for a quick reconnect
        s_bookoo_write_char   = nullptr;
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
        bookoo_push1(BOOKOO_CMD_TARE);
    } else {
        Serial.println("[scale] tare: Felicita command sequence not yet documented");
    }
}

void scale_tare_and_start() {
    bookoo_push1(BOOKOO_CMD_TARE_AND_START);
}

void scale_timer_stop() {
    bookoo_push1(BOOKOO_CMD_STOP);
}

void scale_timer_reset() {
    // Nothing to clear: skip the write (and its unverified side effects) when
    // the clock already reads 0:00.
    if (scale.timer_ms == 0) return;
    bookoo_push1(BOOKOO_CMD_RESET);
}

void scale_timer_reset_forced() {
    // No timer_ms guard (v0.53): used for the delayed reset ~2 s after the lever
    // returns. The reset must go out whatever the reported clock reads (in Auto
    // mode STOP had already zeroed it while the display still flashed), so the
    // guarded scale_timer_reset() could be a silent no-op exactly when needed.
    // bookoo_push() still requires a connected Bookoo; the drain logs the send.
    bookoo_push1(BOOKOO_CMD_RESET);
}

void scale_reset_and_start() {
    // RESET is queued only when the scale still shows a non-zero clock, then
    // TARE_AND_START. A delayed reset still pending from the previous lever
    // return is cancelled at brew start by the UI before this is called.
    // One push, so the pair is queued all-or-nothing.
    if (scale.timer_ms != 0) {
        const uint8_t cmds[2] = { BOOKOO_CMD_RESET, BOOKOO_CMD_TARE_AND_START };
        bookoo_push(cmds, 2);
    } else {
        bookoo_push1(BOOKOO_CMD_TARE_AND_START);
    }
}

uint32_t scale_timer_ms() {
    return scale.timer_ms;
}

bool scale_connected() {
    return scale.connected;
}

float scale_weight() {
    return scale.weight_g;
}

float scale_flow() {
    return scale.flow_gps;
}

uint32_t scale_weight_age_ms() {
    uint32_t t = scale.last_weight_ms;
    if (t == 0) return UINT32_MAX;   // no weight notify received yet this session
    return millis() - t;
}

ScaleLink scale_link_state() {
    return s_link;
}

const char* scale_model_name() {
    switch (scale.model) {
        case SCALE_FELICITA_ARC:    return "Felicita Arc";
        case SCALE_BOOKOO_THEMIS:   return "Bookoo Themis Ultra";
        default:                    return "—";
    }
}

// User flipped the settings toggle. The scale task reads settings.scale_ble_enabled
// directly on its own poll (park gate + stay-connected loop), so this only handles
// the side-effects: log the change, and on re-enable open the fast-scan window so a
// scale reconnects quickly even if it has been off longer than RECONNECT_FAST_WIN_MS.
void scale_set_enabled(bool enabled) {
    if (enabled) {
        s_last_disconnect_ms = millis();
        wlogf("[scale] BT enabled by user\n");
    } else {
        wlogf("[scale] BT disabled by user\n");
    }
}

void scale_kick_fast_scan() {
    // Open the fast-scan reconnect window (same signal a disconnect raises) and
    // wake the task's paused inter-scan wait. If the task is parked (brew/OTA/
    // standby/BT-off) this just pre-arms both; scanning resumes when it unparks.
    s_last_disconnect_ms = millis();
    s_scan_kick          = true;
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

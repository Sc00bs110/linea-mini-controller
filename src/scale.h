#pragma once
#include <stdint.h>
#include <stdbool.h>

enum ScaleModel { SCALE_NONE, SCALE_FELICITA_ARC, SCALE_BOOKOO_THEMIS };

struct ScaleState {
    ScaleModel    model;
    bool          connected;
    volatile float    weight_g;     // grams — updated from BLE notification (core 0)
    volatile float    flow_gps;     // g/s — Bookoo only
    volatile uint32_t timer_ms;     // scale timer — Bookoo only
    volatile uint8_t  battery_pct;  // 0–100 — Bookoo only
    bool          stable;
    volatile uint32_t last_weight_ms;  // millis() of last weight notify (0 = none yet)
};

extern ScaleState scale;

// Call once after lv_init, before WiFi — starts BLE scan task on core 0
void  scale_init();

// Send tare command to active scale
void  scale_tare();

// Bookoo only: tare weight and start timer atomically (use at shot start)
void  scale_tare_and_start();

bool        scale_connected();
float       scale_weight();         // current weight in grams (0.0 if disconnected)
// Milliseconds since the last weight notification (UINT32_MAX if none received
// yet). Used by the UI's brew-by-weight failsafe to detect a stalled scale feed.
uint32_t    scale_weight_age_ms();
const char* scale_model_name();     // "Felicita Arc" / "Bookoo Themis Ultra" / "—"

// Apply a change to the user's scale-BT enable toggle. The scale task polls
// settings.scale_ble_enabled itself (parks when OFF, drops a live link); this
// logs the change and, on re-enable, re-opens the fast-scan reconnect window.
void scale_set_enabled(bool enabled);

// Force an immediate reconnect attempt from the UI/loop task (the scale
// status pill on the main screen). Opens the fast-scan reconnect window
// (sets s_last_disconnect_ms) AND raises a volatile kick flag so the scale
// task cuts short any long relaxed inter-scan pause and starts scanning
// within one ~500 ms slice. No-op effect if the task is parked (brew/OTA/
// standby/BT-off); it takes effect as soon as the task leaves its gate.
// Thread-safe: the flag is written here (loop core) and read on the BLE task.
void scale_kick_fast_scan();

// Suspend/resume BLE scanning while an OTA transfer runs — scanning on the
// shared 2.4GHz radio stalls the OTA TCP stream (proven live 2026-07-08:
// transfers died mid-stream exactly while "[scale] scanning" was active).
void scale_set_ota_hold(bool hold);

// Suspend the 2.4GHz radio for BLE's sake during a foreground TLS/OTA operation.
// Pausing the scan is not enough on the S3's single shared radio — this parks
// the scale task, stops any active scan, disconnects a connected client, then
// NimBLEDevice::deinit()s to free the radio entirely so WiFi.setSleep(false)
// becomes legal and the TLS handshake gets the ~50KB of internal RAM it needs.
// Sequencing (race-free): hold → stop scan → wait for park → deinit(true). The
// deinit runs strictly AFTER the task has parked (no NimBLE call in flight).
// Safe to call from another task while the scale task is mid-scan/mid-connect.
void scale_radio_suspend();

// Undo scale_radio_suspend(): re-init NimBLE, then release the hold so the parked
// scale task resumes scanning and reconnects to the scale naturally. Sequencing
// (race-free): re-init BLE → clear hold. The re-init runs strictly BEFORE the
// task unparks, so the task never makes a NimBLE call against a torn-down stack.
// For OTA-check callers that must restore BLE afterwards; install/ArduinoOTA
// callers reboot instead and use scale_radio_release().
void scale_radio_resume();

// Alias for scale_radio_suspend(), kept for the reboot-ending OTA paths
// (install / ArduinoOTA) that release the radio and never resume — the reboot
// restores BLE from scratch, so no scale_radio_resume() is needed there.
void scale_radio_release();

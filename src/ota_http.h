#pragma once

#include <stdint.h>   // uint8_t (ota_http_progress / ota_http_set_progress)

// HTTP(S)-pull OTA. Two independent entry points share the same queued-URL
// machinery serviced from loop():
//   1. MQTT-triggered LAN pull (plain http://, build-PC path).
//   2. GitHub release install (https://, guarded by FEATURE_GH_OTA).
// The MQTT callback runs on the loop task and must not perform the (long,
// blocking, radio-reconfiguring) update inline, so requests are queued here.

// Queue an HTTP(S)-pull firmware update from `url`. Copies the URL into a static
// buffer and sets a pending flag. Safe to call from the MQTT callback.
void ota_http_request(const char* url);

// Call from loop(). When a request is pending: guards against re-entrancy,
// switches on the OTA progress screen (ui_ota_start, from this loop context),
// then spawns a task that streams the image manually. Plain http:// uses a
// WiFiClient; https:// uses WiFiClientSecure + setInsecure() and follows
// redirects (only compiled when FEATURE_GH_OTA is defined — brings in the TLS
// stack). On success the device reboots into the new image; on any failure it
// logs the reason and reboots.
void ota_http_tick();

// Transfer progress, 0..100. Published by the transfer task and polled by the
// UI (loop task). Meaningful on both envs (plain-HTTP LAN and TLS GitHub paths).
uint8_t ota_http_progress();

// Feed progress from an external pusher (main.cpp's ArduinoOTA onProgress hook),
// so the same on-screen percent readout covers the dev-PC push path too.
void ota_http_set_progress(uint8_t pct);

#if defined(FEATURE_GH_OTA)
// ─── GitHub release check + install ──────────────────────────────────────────
// State exposed to the UI. The check runs in a short-lived FreeRTOS task, so
// the state and remote-version string are written there and read by the UI task.
enum GhOtaState {
    GH_IDLE,          // nothing checked yet this boot
    GH_CHECKING,      // check task running
    GH_UP_TO_DATE,    // remote version <= FW_VERSION
    GH_UPDATE_AVAIL,  // remote version > FW_VERSION (install offered)
    GH_CHECK_FAILED,  // network/parse error
};

// Spawn the check task: fetches GH_LATEST_BASE "version.json", parses "version",
// compares numerically against FW_VERSION, and updates the state below. No-op if
// a check is already running. Does NOT release the radio (keeps the BLE scale
// alive during a background check, unlike the install path).
void ota_gh_check();

// Queue the GitHub firmware.bin for install via ota_http_request(). The existing
// OTA screen takes over from ota_http_tick()'s ui_ota_start() call.
void ota_gh_install();

GhOtaState  ota_gh_state();            // current check state (UI reads this)
const char* ota_gh_remote_version();   // last fetched remote version, "" if none
#endif  // FEATURE_GH_OTA

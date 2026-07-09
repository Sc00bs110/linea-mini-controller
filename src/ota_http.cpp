#include "ota_http.h"
#include "scale.h"
#include "ui.h"
#include "wlog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPUpdate.h>   // Arduino-ESP32 3.x bundled lib; global `httpUpdate`.

#if defined(FEATURE_GH_OTA)
#include "version.h"          // FW_VERSION — the local version we compare against
#include <WiFiClientSecure.h> // TLS client for the GitHub release fetch/install
#include <HTTPClient.h>       // version.json GET
#endif

// Firmware URL requested via MQTT, serviced from loop() (never inside the MQTT
// callback). Sized for a typical "http://<host>:<port>/firmware.bin".
static char          s_pending_url[160] = {};
static volatile bool s_pending          = false;

void ota_http_request(const char* url) {
    if (!url) return;
    strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    s_pending = true;
}

// Runs the actual transfer. Lives in its own dedicated task (not loop()):
// the TLS install path needs ~16 KB of stack for the mbedtls handshake, and
// fattening the loop task's stack for a once-per-update event would cost that
// internal RAM permanently.
static void ota_run_task(void*) {
    bool is_http  = (strncmp(s_pending_url, "http://", 7) == 0);
    bool is_https = (strncmp(s_pending_url, "https://", 8) == 0);

#if !defined(FEATURE_GH_OTA)
    // TLS not compiled in on this env — only the plain-HTTP LAN path exists.
    is_https = false;
#endif
    if (!is_http && !is_https) {
        wlogf("[ota-http] ignoring unsupported URL: %s\n", s_pending_url);
        vTaskDelete(NULL);
        return;   // not reached; silences no-return analysis
    }

    wlogf("[ota-http] starting %s OTA from %s\n",
          is_https ? "HTTPS" : "HTTP", s_pending_url);
    ui_ota_start();   // arg-less; no percent API exists, so no progress hook

    // WiFi and BLE share the S3's single 2.4GHz radio: BLE must be fully
    // deinitialized (not merely paused) for the transfer to survive. This also
    // makes WiFi.setSleep(false) legal (it throws while NimBLE is initialized).
    scale_radio_release();
    WiFi.setSleep(false);

    httpUpdate.rebootOnUpdate(true);    // success path reboots into new image

    t_httpUpdate_return ret;
#if defined(FEATURE_GH_OTA)
    if (is_https) {
        // GitHub 302-redirects release assets to its CDN, so follow redirects.
        // setInsecure(): no cert validation. Accepted tradeoff — the dual-
        // partition scheme rolls back on a corrupt/incomplete image, so a MITM
        // can at worst cause a rejected update, not a persistently bad boot.
        WiFiClientSecure secure;
        secure.setInsecure();
        secure.setTimeout(30);          // seconds
        httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        ret = httpUpdate.update(secure, s_pending_url);
    } else
#endif
    {
        WiFiClient client;
        // Generous socket timeout so a slow build-PC response doesn't abort the
        // pull; the task WDT is not armed on the loop task, so this is WDT-safe.
        client.setTimeout(30);          // seconds
        ret = httpUpdate.update(client, s_pending_url);
    }

    // Only reached on failure/no-update — a successful update reboots inside
    // update(). Reboot unconditionally so any wedged Update state machine is
    // cleared (arduino-esp32 #8393) and BLE/radio come back clean next boot.
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            wlogf("[ota-http] failed (%d): %s\n",
                  httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            wlogf("[ota-http] server reported no update\n");
            break;
        case HTTP_UPDATE_OK:
            // Unreachable with rebootOnUpdate(true), but log for completeness.
            wlogf("[ota-http] update ok — rebooting\n");
            break;
    }
    ESP.restart();
}

void ota_http_tick() {
    if (!s_pending) return;
    s_pending = false;
    // 16 KB stack: mbedtls handshake headroom on the TLS path (plain HTTP needs
    // far less, but one size keeps it simple). Every outcome ends in a reboot,
    // so the task never needs to be tracked or joined.
    if (xTaskCreate(ota_run_task, "ota_run", 16384, NULL, 2, NULL) != pdPASS) {
        wlogf("[ota-http] failed to spawn transfer task\n");
    }
}

#if defined(FEATURE_GH_OTA)
// ─── GitHub release check + install ──────────────────────────────────────────

// Latest-release asset base. GitHub serves version.json and firmware.bin from
// here, 302-redirecting each to its objects CDN (handled via follow-redirects).
#define GH_LATEST_BASE \
    "https://github.com/Sc00bs110/linea-mini-controller/releases/latest/download/"

// Written by the check task, read by the UI task — hence volatile. The version
// string is only read by the UI after the task publishes GH_UPDATE_AVAIL, so a
// plain buffer with the state as the publish barrier is sufficient.
static volatile GhOtaState s_gh_state = GH_IDLE;
static char                s_gh_remote_ver[16] = {};

GhOtaState  ota_gh_state()          { return s_gh_state; }
const char* ota_gh_remote_version() { return s_gh_remote_ver; }

// Parse a "vMAJ.MIN" version into a single comparable integer (major*1000+minor).
// Returns false if the string does not start with the expected shape. Minor is
// parsed as an integer so v0.9 (9) correctly sorts below v0.25 (25).
static bool gh_parse_version(const char* s, long* out) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    int maj = 0, min = 0;
    if (sscanf(s, "%d.%d", &maj, &min) != 2) return false;
    *out = (long)maj * 1000 + min;
    return true;
}

// FreeRTOS task body: fetch the manifest, parse it, compare, publish state.
static void gh_check_task(void*) {
    s_gh_state = GH_CHECKING;
    s_gh_remote_ver[0] = '\0';

    WiFiClientSecure secure;
    secure.setInsecure();   // see the tradeoff note on the install path above
    secure.setHandshakeTimeout(30);   // seconds — TLS over a laggy coex link
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub -> CDN 302
    http.setConnectTimeout(20000);   // ms
    http.setTimeout(20000);   // ms

    // TEMP diag while the check path is being field-proven.
    wlogf("[ota-gh] heap free=%u largest=%u internal=%u rssi=%d\n",
          (unsigned)esp_get_free_heap_size(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (int)WiFi.RSSI());

    GhOtaState result = GH_CHECK_FAILED;
    if (http.begin(secure, GH_LATEST_BASE "version.json")) {
        int code = http.GET();
        if (code < 0) {
            char ebuf[128] = {};
            secure.lastError(ebuf, sizeof(ebuf));
            wlogf("[ota-gh] tls lastError: %s\n", ebuf[0] ? ebuf : "(none)");
        }
        if (code == HTTP_CODE_OK) {
            String body = http.getString();
            // Minimal JSON scan — no ArduinoJson dependency. Expect
            // {"version":"v0.25"}; pull the value after the "version" key.
            const char* p = strstr(body.c_str(), "\"version\"");
            char remote[16] = {};
            if (p && sscanf(p, "\"version\"%*[^\"]\"%15[^\"]\"", remote) == 1) {
                long local_v = 0, remote_v = 0;
                bool have_local  = gh_parse_version(FW_VERSION, &local_v);
                bool have_remote = gh_parse_version(remote, &remote_v);
                if (have_local && have_remote) {
                    strncpy(s_gh_remote_ver, remote, sizeof(s_gh_remote_ver) - 1);
                    result = (remote_v > local_v) ? GH_UPDATE_AVAIL : GH_UP_TO_DATE;
                    wlogf("[ota-gh] local=%s remote=%s -> %s\n",
                          FW_VERSION, remote,
                          result == GH_UPDATE_AVAIL ? "update available" : "up to date");
                } else {
                    wlogf("[ota-gh] unparseable version (local=%s remote=%s)\n",
                          FW_VERSION, remote);
                }
            } else {
                wlogf("[ota-gh] no \"version\" field in manifest\n");
            }
        } else {
            wlogf("[ota-gh] HTTP GET failed: %d\n", code);
        }
        http.end();
    } else {
        wlogf("[ota-gh] connection to GitHub failed\n");
    }

    s_gh_state = result;   // publish last (acts as the release barrier)
    vTaskDelete(NULL);
}

void ota_gh_check() {
    if (s_gh_state == GH_CHECKING) return;   // one check at a time
    // TLS handshake wants generous stack headroom (loop's ota_task uses 8192).
    if (xTaskCreate(gh_check_task, "gh_check", 12288, NULL, 2, NULL) != pdPASS) {
        wlogf("[ota-gh] failed to spawn check task\n");
        s_gh_state = GH_CHECK_FAILED;
    }
}

void ota_gh_install() {
    // Reuse the queued-URL machinery: ota_http_tick() picks up the https:// URL,
    // switches on the OTA screen, releases the radio, and runs the TLS update.
    ota_http_request(GH_LATEST_BASE "firmware.bin");
}
#endif  // FEATURE_GH_OTA

#include "ota_http.h"
#include "scale.h"
#include "ui.h"
#include "wlog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>   // manual streaming GET (both http:// and https:// paths)
#include <Update.h>       // Update.begin/write/end — write the incoming image

#if defined(FEATURE_GH_OTA)
#include "version.h"          // FW_VERSION — the local version we compare against
#include <WiFiClientSecure.h> // TLS client for the GitHub release fetch/install
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

// Transfer progress (0..100). Written by the transfer task (and by the
// ArduinoOTA onProgress hook via ota_http_set_progress); read by the UI from
// loop() context. volatile: cross-task publish with the reader polling it.
static volatile uint8_t s_ota_pct = 0;

// Re-entrancy guard. Set true once a transfer is spawned and never cleared: the
// transfer always ends in ESP.restart(), so it resets naturally across reboot.
// Prevents a second (radio-releasing, Update-owning) transfer from a stray tap.
static volatile bool s_running = false;

uint8_t ota_http_progress()                { return s_ota_pct; }
void    ota_http_set_progress(uint8_t pct) { s_ota_pct = pct; }

// Runs the actual transfer. Lives in its own dedicated task (not loop()): the
// TLS install path needs ~16 KB of stack for the mbedtls handshake.
//
// Streams the image manually with the EXACT client configuration proven by
// gh_check_task (which reliably completes GitHub GETs in ~5 s), rather than
// httpUpdate.update() — the latter was observed to hang silently for 90+ s
// against the same host (no error return, no reboot). Every exit reboots:
// success boots the new image; any failure reboots to clear a half-written
// Update state (arduino-esp32 #8393) and bring the radio/BLE back clean.
static void ota_run_task(void*) {
    bool is_http  = (strncmp(s_pending_url, "http://", 7) == 0);
    bool is_https = (strncmp(s_pending_url, "https://", 8) == 0);

#if !defined(FEATURE_GH_OTA)
    // TLS not compiled in on this env — only the plain-HTTP LAN path exists.
    is_https = false;
#endif
    if (!is_http && !is_https) {
        wlogf("[ota-http] ignoring unsupported URL: %s\n", s_pending_url);
        ESP.restart();   // s_running was set by the tick — reboot clears it
        return;
    }

    wlogf("[ota-http] starting %s OTA from %s\n",
          is_https ? "HTTPS" : "HTTP", s_pending_url);

    // WiFi and BLE share the S3's single 2.4GHz radio: BLE must be fully
    // deinitialized (not merely paused) for the transfer to survive. This also
    // makes WiFi.setSleep(false) legal (it throws while NimBLE is initialized).
    scale_radio_release();
    WiFi.setSleep(false);

    s_ota_pct = 0;

    // The client objects must outlive the whole transfer — HTTPClient::begin()
    // stores a reference to them — so they live at task scope, not inside the
    // scheme branch below (secure only exists when the TLS stack is compiled in).
#if defined(FEATURE_GH_OTA)
    WiFiClientSecure secure;
#endif
    WiFiClient plain;

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub -> CDN 302
    http.setConnectTimeout(20000);   // ms
    http.setTimeout(20000);          // ms

    // Shared cleanup+reboot for every failure exit. ESP.restart() does not
    // return; the `return;` after each call documents the control flow.
    auto fail = [&](const char* why) {
        wlogf("[ota-http] FAILED: %s\n", why);
        Update.abort();
        http.end();
        ESP.restart();
    };

    bool begun;
#if defined(FEATURE_GH_OTA)
    if (is_https) {
        // setInsecure(): no cert validation. Accepted tradeoff — the dual-
        // partition scheme rolls back on a corrupt/incomplete image, so a MITM
        // can at worst cause a rejected update, not a persistently bad boot.
        secure.setInsecure();
        secure.setHandshakeTimeout(30);   // seconds — TLS over a laggy coex link
        begun = http.begin(secure, s_pending_url);
    } else
#endif
    {
        begun = http.begin(plain, s_pending_url);
    }
    if (!begun) { fail("http.begin() failed"); return; }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        char b[48];
        snprintf(b, sizeof(b), "HTTP GET returned %d", code);
        fail(b);
        return;
    }

    int len = http.getSize();   // GitHub CDN sends Content-Length
    if (len <= 0) { fail("no/zero Content-Length"); return; }

    if (!Update.begin((size_t)len)) {
        char b[80];
        snprintf(b, sizeof(b), "Update.begin (err %u): %s",
                 (unsigned)Update.getError(), Update.errorString());
        fail(b);
        return;
    }

    // Stream in ~4 KB chunks. buf is static (not on the stack): the 16 KB stack
    // is sized for the mbedtls handshake alone, and a 4 KB stack frame would eat
    // into that headroom on the very path this rewrite exists to make robust.
    // Safe because this task is single-shot (s_running-guarded, reboot-ended).
    static uint8_t buf[4096];
    WiFiClient*    stream    = http.getStreamPtr();
    int            remaining = len;
    uint32_t       last_data = millis();

    while (remaining > 0) {
        size_t avail = stream->available();
        if (avail) {
            size_t toread = avail;
            if (toread > sizeof(buf))    toread = sizeof(buf);
            if ((int)toread > remaining) toread = (size_t)remaining;
            int r = stream->readBytes(buf, toread);
            if (r <= 0) { fail("stream read error"); return; }
            if (Update.write(buf, (size_t)r) != (size_t)r) {
                char b[80];
                snprintf(b, sizeof(b), "Update.write (err %u): %s",
                         (unsigned)Update.getError(), Update.errorString());
                fail(b);
                return;
            }
            remaining -= r;
            s_ota_pct  = (uint8_t)(((int64_t)(len - remaining) * 100) / len);
            last_data  = millis();
        } else if (millis() - last_data > 20000UL) {
            fail("stalled — no data for 20 s");
            return;
        } else {
            delay(1);   // yield to the scheduler (feeds the idle-task WDT)
        }
    }

    if (!Update.end(true) || !Update.isFinished()) {
        char b[80];
        snprintf(b, sizeof(b), "Update.end (err %u): %s",
                 (unsigned)Update.getError(), Update.errorString());
        fail(b);
        return;
    }

    s_ota_pct = 100;
    wlogf("[ota-http] complete (%d bytes) — rebooting into new image\n", len);
    ESP.restart();
}

void ota_http_tick() {
    if (!s_pending) return;
    s_pending = false;

    // Re-entrancy guard: a transfer already owns the radio and the Update state
    // machine. Refuse duplicates with a single log line (s_pending was just
    // cleared, so a stray repeated tap logs at most once per request — no spam).
    if (s_running) {
        wlogf("[ota-http] transfer already running — ignoring request\n");
        return;
    }
    s_running = true;

    // Drive the OTA progress screen from loop() context. LVGL is not thread-safe
    // and the transfer task makes NO LVGL calls, so this is the one place the
    // OTA screen is switched on; the task only publishes s_ota_pct for the UI.
    ui_ota_start();

    // 16 KB stack: mbedtls handshake headroom on the TLS path (plain HTTP needs
    // far less, but one size keeps it simple). Every outcome ends in a reboot,
    // so the task never needs to be tracked or joined.
    if (xTaskCreate(ota_run_task, "ota_run", 16384, NULL, 2, NULL) != pdPASS) {
        wlogf("[ota-http] failed to spawn transfer task — rebooting\n");
        ESP.restart();   // clean recovery (matches the every-outcome-reboots rule)
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

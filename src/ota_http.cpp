#include "ota_http.h"
#include "scale.h"
#include "ui.h"
#include "wlog.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPUpdate.h>   // Arduino-ESP32 3.x bundled lib; global `httpUpdate`.

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

void ota_http_tick() {
    if (!s_pending) return;
    s_pending = false;

    // Only plain HTTP is supported (no TLS in this build). Reject anything else
    // rather than hand a bad URL to HTTPUpdate.
    if (strncmp(s_pending_url, "http://", 7) != 0) {
        wlogf("[ota-http] ignoring non-http URL: %s\n", s_pending_url);
        return;
    }

    wlogf("[ota-http] starting HTTP OTA from %s\n", s_pending_url);
    ui_ota_start();   // arg-less; no percent API exists, so no progress hook

    // WiFi and BLE share the S3's single 2.4GHz radio: BLE must be fully
    // deinitialized (not merely paused) for the transfer to survive. This also
    // makes WiFi.setSleep(false) legal (it throws while NimBLE is initialized).
    scale_radio_release();
    WiFi.setSleep(false);

    WiFiClient client;
    // Generous socket timeout so a slow build-PC response doesn't abort the
    // pull; the task WDT is not armed on the loop task, so this is WDT-safe.
    client.setTimeout(30);              // seconds
    httpUpdate.rebootOnUpdate(true);    // success path reboots into new image

    t_httpUpdate_return ret = httpUpdate.update(client, s_pending_url);

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

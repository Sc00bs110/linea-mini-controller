#pragma once

// MQTT-triggered HTTP-pull OTA. The MQTT callback runs on the loop task and
// must not perform the (long, blocking, radio-reconfiguring) update inline, so
// requests are queued here and serviced from loop().

// Queue an HTTP-pull firmware update from `url`. Copies the URL into a static
// buffer and sets a pending flag. Safe to call from the MQTT callback.
void ota_http_request(const char* url);

// Call from loop(). When a request is pending: releases the radio (BLE deinit),
// disables WiFi sleep, and runs the HTTPUpdate. On success the device reboots
// into the new image; on any failure it logs and reboots for a clean state.
void ota_http_tick();

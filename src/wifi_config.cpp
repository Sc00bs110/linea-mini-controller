// Phase 3 stub -- real BLE/WiFi/MQTT logic ported in Phase 6.
//
// The real wifi_config.cpp reads/writes STA credentials in NVS. Phase 3 does not
// bring up WiFi, so this stub reports "no credentials" and empty SSID/password.
// The signatures match wifi_config.h exactly so ui.cpp compiles/links unchanged.

#include "wifi_config.h"
#include <stdint.h>  // for wifi_retry_count()'s uint32_t return type

void wifi_config_init() {
    // No NVS credential load in Phase 3.
}

bool wifi_config_has_credentials() {
    return false;
}

const char* wifi_config_ssid() {
    return "";
}

const char* wifi_config_pass() {
    return "";
}

void wifi_config_save(const char* ssid, const char* pass) {
    (void)ssid;
    (void)pass;
    // No-op in Phase 3.
}

void wifi_config_clear() {
    // No-op in Phase 3.
}

// Declared `extern uint32_t wifi_retry_count();` in ui.cpp (no header). In the
// full firmware it lives in main.cpp's WiFi driver; Phase 3 has no WiFi, so it is
// defined here alongside the rest of the WiFi-config stub and reports zero retries.
uint32_t wifi_retry_count() {
    return 0;
}

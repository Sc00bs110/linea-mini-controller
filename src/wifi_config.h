#pragma once

// WiFi credential storage in NVS.
// Overrides secrets.h WIFI_SSID / WIFI_PASSWORD at runtime.
// Falls back to secrets.h values if NVS has no saved credentials.

void        wifi_config_init();               // load from NVS
bool        wifi_config_has_credentials();    // true if NVS contains saved SSID
const char* wifi_config_ssid();               // current SSID (NVS or secrets.h)
const char* wifi_config_pass();               // current password
void        wifi_config_save(const char* ssid, const char* pass);
void        wifi_config_clear();              // remove NVS credentials

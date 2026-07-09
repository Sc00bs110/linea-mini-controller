#pragma once
#include <stdbool.h>
#include <stdint.h>

// MQTT broker configuration storage in NVS.
//
// Mirrors the wifi_config pattern: NVS-first, secrets.h fallback. CI-built
// firmware ships with blank MQTT_* defines (see secrets.h.example), so the
// broker config MUST survive such over-the-air updates — hence it lives in NVS,
// not just the compiled-in defines.
//
// Seeding rule (applied once on boot in mqtt_config_init):
//   - NVS has a stored host        -> always use NVS (compiled defines ignored).
//   - NVS empty + MQTT_HOST set     -> seed NVS from secrets.h, then use it.
//   - NVS empty + MQTT_HOST empty   -> MQTT stays disabled (no host).

void        mqtt_config_init();      // load from NVS, seeding from secrets.h if needed
bool        mqtt_config_enabled();   // true when a non-empty broker host is configured
const char* mqtt_config_host();      // broker host (NVS or secrets.h); "" if unset
uint16_t    mqtt_config_port();      // broker port (defaults to 1883 if unset)
const char* mqtt_config_user();      // broker username ("" = anonymous)
const char* mqtt_config_pass();      // broker password

#include "mqtt_config.h"
#include "secrets.h"   // MQTT_HOST/PORT/USER/PASS as compile-time fallback + seed
#include "wlog.h"
#include <Preferences.h>
#include <string.h>

// Buffers sized for typical broker settings; oversized values are truncated.
static char     _host[64] = {};
static char     _user[48] = {};
static char     _pass[64] = {};
static uint16_t _port     = 1883;

void mqtt_config_init() {
    Preferences prefs;
    prefs.begin("mqttcfg", true);   // read-only
    bool has_nvs = prefs.isKey("host");
    if (has_nvs) {
        prefs.getString("host", _host, sizeof(_host));
        prefs.getString("user", _user, sizeof(_user));
        prefs.getString("pass", _pass, sizeof(_pass));
        _port = prefs.getUShort("port", 1883);
    }
    prefs.end();

    if (has_nvs) {
        wlogf("[mqtt-cfg] using NVS broker %s:%u\n", _host, _port);
        return;
    }

    // No stored config. Seed from the compiled-in secrets.h values ONLY if a
    // real host was baked in — a CI-stubbed build has MQTT_HOST == "" and must
    // never seed NVS with a bogus host (leaves MQTT disabled instead).
    if (strlen(MQTT_HOST) > 0) {
        strncpy(_host, MQTT_HOST, sizeof(_host) - 1);
        strncpy(_user, MQTT_USER, sizeof(_user) - 1);
        strncpy(_pass, MQTT_PASS, sizeof(_pass) - 1);
        _port = MQTT_PORT;

        Preferences w;
        w.begin("mqttcfg", false);
        w.putString("host", _host);
        w.putString("user", _user);
        w.putString("pass", _pass);
        w.putUShort("port", _port);
        w.end();
        wlogf("[mqtt-cfg] seeded NVS from secrets.h (%s:%u)\n", _host, _port);
    } else {
        wlogf("[mqtt-cfg] no broker configured — MQTT disabled\n");
    }
}

bool        mqtt_config_enabled() { return _host[0] != '\0'; }
const char* mqtt_config_host()    { return _host; }
uint16_t    mqtt_config_port()    { return _port; }
const char* mqtt_config_user()    { return _user; }
const char* mqtt_config_pass()    { return _pass; }

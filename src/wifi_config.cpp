#include "wifi_config.h"
#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD as compile-time fallback
#include <Preferences.h>
#include <string.h>

static char _ssid[33] = {};
static char _pass[65] = {};
static bool _has_nvs  = false;

void wifi_config_init() {
    Preferences prefs;
    prefs.begin("wifi", true);
    _has_nvs = prefs.isKey("ssid");
    if (_has_nvs) {
        prefs.getString("ssid", _ssid, sizeof(_ssid));
        prefs.getString("pass", _pass, sizeof(_pass));
    }
    prefs.end();
    // Seed NVS once from a locally-built secrets.h so CI-built OTA images
    // (blank WIFI_SSID stub) inherit working credentials — same rule as
    // mqtt_config. A CI build never seeds because its compiled SSID is empty.
    if (!_has_nvs && strlen(WIFI_SSID) > 0)
        wifi_config_save(WIFI_SSID, WIFI_PASSWORD);
}

bool wifi_config_has_credentials() { return _has_nvs; }

const char* wifi_config_ssid() { return _has_nvs ? _ssid : WIFI_SSID;     }
const char* wifi_config_pass() { return _has_nvs ? _pass : WIFI_PASSWORD; }

void wifi_config_save(const char* ssid, const char* pass) {
    strncpy(_ssid, ssid, 32); _ssid[32] = '\0';
    strncpy(_pass, pass, 64); _pass[64] = '\0';
    _has_nvs = true;
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", _ssid);
    prefs.putString("pass", _pass);
    prefs.end();
}

void wifi_config_clear() {
    _has_nvs = false;
    _ssid[0] = '\0';
    _pass[0] = '\0';
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
}

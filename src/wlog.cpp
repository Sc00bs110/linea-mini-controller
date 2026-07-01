#include "wlog.h"

// Phase 1 minimal shim: Serial-only logging so gicar.cpp (and any other module
// that calls wlogf) can be ported unchanged without pulling in WiFi.
//
// The full WiFi telnet-log implementation from W:\LM_Mini.git (a WiFiServer on
// port 4444) is a later-phase concern; it will simply overwrite this file when
// the WiFi/wlog module is ported. Until then, wlogf routes to the native
// USB-Serial-JTAG console, which is exactly what the Phase 1 bench gate reads.

void wlog_init() { /* no-op — no TCP log server in the Serial-only shim */ }

void wlog_tick() { /* no-op — no TCP clients to service in the shim */ }

void wlogf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
}

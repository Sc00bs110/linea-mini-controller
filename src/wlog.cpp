#include "wlog.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>

static WiFiServer s_server(4444);
static WiFiClient s_client;
static bool       s_started = false;

void wlog_init() { /* no-op — server starts lazily in wlog_tick() */ }

void wlog_tick() {
    if (!s_started && WiFi.status() == WL_CONNECTED) {
        s_server.begin();
        s_started = true;
        Serial.println("[wlog] telnet log server started on port 4444");
    }
    if (!s_started) return;

    WiFiClient c = s_server.accept();
    if (c) {
        s_client = c;
        Serial.println("[wlog] client connected");
    }
}

void wlogf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.print(buf);
    if (s_client && s_client.connected()) s_client.print(buf);
}

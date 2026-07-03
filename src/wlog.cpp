#include "wlog.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <lwip/sockets.h>

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
    // Only send to the TCP client if the socket is writable right now — with a
    // full TX buffer, NetworkClient::write() blocks up to 10 s (10 retries of a
    // 1 s select), stalling loop() and the GICAR poll cadence (observed live:
    // R-frame rate dropped 3x with a slow client attached). Drop the line
    // instead. availableForWrite() can't be used: NetworkClient never
    // overrides it, so it always returns 0.
    if (s_client && s_client.connected()) {
        int fd = s_client.fd();
        if (fd >= 0) {
            fd_set set;
            struct timeval tv = {0, 0};
            FD_ZERO(&set);
            FD_SET(fd, &set);
            if (select(fd + 1, NULL, &set, NULL, &tv) > 0 && FD_ISSET(fd, &set))
                s_client.write((const uint8_t*)buf, strlen(buf));
        }
    }
}

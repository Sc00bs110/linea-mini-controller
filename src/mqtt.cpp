// Phase 3 stub -- real BLE/WiFi/MQTT logic ported in Phase 6.
//
// The real mqtt.cpp runs a PubSubClient connection publishing Home Assistant
// discovery + state topics. Phase 3 does not bring up networking, so this stub
// reports a permanently-disconnected broker. Signatures match mqtt.h exactly so
// ui.cpp's status line compiles/links unchanged.

#include "mqtt.h"

void mqtt_init() {
    // No broker connection in Phase 3.
}

void mqtt_tick() {
    // Nothing to service in Phase 3.
}

bool mqtt_connected() {
    return false;
}

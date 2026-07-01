// Phase 3 stub -- real BLE/WiFi/MQTT logic ported in Phase 6.
//
// The real wifi_ap.cpp runs a SoftAP + DNS captive portal + HTTP server for
// credential provisioning. Phase 3 does not bring up WiFi, so this stub never
// activates the AP. Signatures match wifi_ap.h exactly so ui.cpp's
// provisioning-screen button callbacks compile/link unchanged.

#include "wifi_ap.h"

void wifi_ap_start() {
    // No SoftAP / captive portal in Phase 3.
}

void wifi_ap_handle() {
    // AP never active in Phase 3 -- nothing to service.
}

bool wifi_ap_active() {
    return false;
}

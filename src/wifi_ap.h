#pragma once

#define WIFI_AP_SSID "LM-Mini-Setup"   // open AP for captive portal
#define WIFI_AP_IP   "192.168.4.1"

// Start AP + DNS redirect + HTTP captive portal.
// POST /save: saves ssid/pass to NVS and reboots.
void wifi_ap_start();

// Call every loop() while AP mode is active.
void wifi_ap_handle();

bool wifi_ap_active();

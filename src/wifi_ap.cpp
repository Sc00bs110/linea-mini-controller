#include "wifi_ap.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

static WebServer  ap_server(80);
static DNSServer  dns_server;
static bool       s_active = false;

// ─── HTML pages ───────────────────────────────────────────────────────────────

static const char HTML_FORM[] PROGMEM = R"html(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LM Mini WiFi Setup</title>
<style>
body{font-family:Arial,sans-serif;background:#111;color:#eee;padding:24px;max-width:400px;margin:0 auto}
h2{color:#D4891A;margin-bottom:24px}
label{display:block;margin-bottom:4px;color:#aaa;font-size:14px}
input{width:100%;padding:10px;margin-bottom:16px;background:#222;border:1px solid #444;
      color:#fff;border-radius:4px;font-size:16px;box-sizing:border-box}
button{width:100%;padding:14px;background:#27ae60;color:#fff;border:none;
       border-radius:4px;font-size:16px;cursor:pointer}
button:hover{background:#2ecc71}
.note{font-size:12px;color:#666;margin-top:16px;text-align:center}
</style></head>
<body>
<h2>La Marzocco Mini<br>WiFi Setup</h2>
<form method="POST" action="/save">
<label>WiFi Network Name (SSID)</label>
<input name="ssid" type="text" placeholder="Your WiFi name" autocomplete="off">
<label>Password</label>
<input name="pass" type="password" placeholder="WiFi password" autocomplete="off">
<button type="submit">Save &amp; Connect</button>
</form>
<p class="note">Device will restart and connect to your WiFi.<br>
This page will become unavailable.</p>
</body></html>
)html";

static const char HTML_SAVED[] PROGMEM = R"html(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>
body{font-family:Arial,sans-serif;background:#111;color:#eee;padding:24px;max-width:400px;margin:0 auto;text-align:center}
h2{color:#27ae60} p{color:#aaa;margin-top:16px}
</style></head>
<body>
<h2>WiFi Settings Saved!</h2>
<p>LM Mini is restarting and connecting to your network.<br><br>
You can close this page.</p>
</body></html>
)html";

// ─── Request handlers ─────────────────────────────────────────────────────────

static void handle_root() {
    ap_server.send_P(200, "text/html", HTML_FORM);
}

static void handle_save() {
    String ssid = ap_server.arg("ssid");
    String pass = ap_server.arg("pass");

    if (ssid.length() == 0) {
        ap_server.send(400, "text/plain", "SSID cannot be empty");
        return;
    }

    wifi_config_save(ssid.c_str(), pass.c_str());
    ap_server.send_P(200, "text/html", HTML_SAVED);

    Serial.printf("[ap] Saved SSID: %s — restarting\n", ssid.c_str());
    delay(2000);
    ESP.restart();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void wifi_ap_start() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID);   // open — no password for easy access

    // Redirect all DNS queries to our IP (captive portal trigger on phones)
    dns_server.start(53, "*", WiFi.softAPIP());

    ap_server.on("/",      HTTP_GET,  handle_root);
    ap_server.on("/save",  HTTP_POST, handle_save);
    ap_server.onNotFound(handle_root);   // redirect 404 → form
    ap_server.begin();

    s_active = true;
    Serial.printf("[ap] Hotspot started: SSID=\"%s\"  IP=%s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void wifi_ap_handle() {
    dns_server.processNextRequest();
    ap_server.handleClient();
}

bool wifi_ap_active() { return s_active; }

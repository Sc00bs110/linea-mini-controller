#pragma once
#include <Arduino.h>

// RAM-only per-shot flow/weight curve recorder. Mirrors shot_log's edge-detect
// lifecycle but keeps nothing on disk: the finished curve is handed to mqtt.cpp
// as a JSON blob and then dropped.

void   flow_log_update();       // call every loop() — edge-detects off machine.brew_active
bool   flow_log_has_pending();  // true once a finished shot's curve is ready to publish
String flow_log_take_json();    // builds + returns pending JSON, clears the pending flag (call once)

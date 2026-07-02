#pragma once
#include <Arduino.h>

void shot_log_init();           // call from setup() after LittleFS.begin()
void shot_log_update();         // call every loop() — detects shot start/end, records samples

// HTTP API helpers
String shot_log_list_json(int limit = 20); // JSON array of shot summaries
String shot_log_get_json(uint32_t shot_id); // full shot JSON with samples
int    shot_log_count();

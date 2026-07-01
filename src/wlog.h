#pragma once
#include <Arduino.h>

void wlog_init();          // call once after WiFi connected
void wlog_tick();          // call from loop() to accept new clients
void wlogf(const char* fmt, ...);  // printf to Serial + TCP client

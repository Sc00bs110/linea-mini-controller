#pragma once
#include <stdbool.h>

void mqtt_init();       // call once after WiFi connects
void mqtt_tick();       // call every loop()
bool mqtt_connected();

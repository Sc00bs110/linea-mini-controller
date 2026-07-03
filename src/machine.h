#pragma once
#include <stdint.h>
#include <stdbool.h>

struct MachineState {
    // From R-frame poll responses (~760 ms cadence)
    float    coffee_temp_c;    // live boiler reading (payload[28..29] / 10.0)
    float    setpoint_c;       // setpoint read at boot from 0x0007 register
    bool     brew_active;      // payload[17] == 0x01
    bool     steam_active;     // boiler_flags bit 4 (BF_STEAM_BOILER_ON)
    bool     heating_element;  // boiler_flags bit 0
    bool     pump_active;      // boiler_flags bit 1
    uint8_t  boiler_flags;     // full payload[27] bitmask
    bool     standby;          // true while in full standby (0x0000 register)

    // From autonomous Z-frames (~11 Hz during brew/backflush)
    float    z_temp_c;         // Z-frame temperature (resolution /160.0)
    bool     z_shot_active;    // Z-frame pair[14] == 0x10

    // Shot tracking
    uint32_t shot_start_ms;    // millis() when last shot started (0 = not brewing)

    // Connectivity
    uint32_t last_frame_ms;    // millis() of last valid R-frame or Z-frame
    bool     connected;        // false if >3 s since last frame
};

extern MachineState machine;

void machine_init();
void machine_update();               // call every loop()

// Commands — all verified from dual-channel logic analyser captures
void machine_set_temp(float celsius);        // 85–100°C; register 0x0007 (uint16 BE, *10)
void machine_set_steam(bool on);             // 0x00E1: 0x81=on, 0x01=off
void machine_set_standby(bool standby);      // 0x0000: 0x00=standby, 0x01=wake (verified)
void machine_set_preinfusion(bool on);       // 0x000B: 0x01=on, 0x00=off
void machine_set_preinfusion_dur(uint8_t v); // 0x00E2: 0x14=20 (unit TBD, app range 2–20)
// Clean cycle: start pulses 0x00E1=0x82 at 1 Hz from machine_update() until the
// cycle's brew phase completes, stop is called, or the 140 s window expires.
// (A single 0x82 write does NOT engage the cycle.)
void machine_clean_start();
void machine_clean_stop();
bool machine_clean_active();
bool machine_clean_ready();   // burst done — lever may be lifted now
void machine_brew_stop();                    // standby+wake: confirmed empirically 2026-06-25

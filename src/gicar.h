#pragma once
#include <Arduino.h>

// UART1 pins — Firebeetle 2 ESP32-C6: GPIO2 TX, GPIO3 RX
// Moved off GPIO16/17 during Phase 5 bring-up: GPIO16/17 produced zero bytes
// ever received despite wiring/continuity/loopback all checking out -- likely
// a cold joint or similar physical defect at that specific header location on
// this board, not a protocol/software issue. GPIO2/3 carry no strapping/JTAG/
// ADC conflicts that block UART use (see
// reference/dfrobot/firebeetle2-esp32-c6-v1.2.md).
//
// CONFIRMED (2026-07-02, live machine): GPIO3=RX/GPIO2=TX is the correct
// mapping -- 432+ R-frames parsed cleanly at ~700 B/s, temp/boiler-flag
// values matching the machine's real behavior. (A passive sniffer used
// during bring-up had decoded our own poll on GPIO2, which pointed at the
// opposite mapping -- that pointer was wrong; live frame parsing is the
// ground truth here.)
// invert=true: CN11 is idle-LOW inverted LVCMOS; pull-up level-shifts but does not invert
#define GICAR_RX_PIN    3   // CN11 TXD → ESP RX (10kΩ pull-up + series R)
#define GICAR_TX_PIN    2   // ESP TX → CN11 RXD (series R)
#define GICAR_BAUD      9600
#define GICAR_BUF_SIZE  128

#define GICAR_Z_FRAME_LEN  55  // autonomous Z6 stream: Z(1)+id(1)+'0000'(4)+data(48)+terminator(1),
                                // confirmed against real captures (reference/lm_mini/gicar_protocol_analysis.md)
#define GICAR_R_FRAME_LEN  81  // poll response R-frames: 1+'4000'+'0023'+35*2+2 = 81 chars

// Boiler flags bitmask (R-frame payload[27])
#define BF_HEATING_ELEMENT  0x01  // heating element active
#define BF_PUMP_ACTIVE      0x02  // pump running (brew)
#define BF_SOLENOID_OPEN    0x08  // brew solenoid open
#define BF_STEAM_BOILER_ON  0x10  // steam boiler enabled

// Boot sequence + UART init.  Sends X-probe, reads machine identity,
// reads initial config block (setpoint at 0x0007 in 0x0000 block).
// Blocks up to ~3 s waiting for X-probe response.
// After gicar_init(), call gicar_process() every loop().
void           gicar_init();

// Call every loop().  Sends R40000023DB poll every 760 ms; drains Serial1
// and parses both R-frames (81 chars) and Z-frames (55 chars).
void           gicar_process();

// ── Z-frame (autonomous 55-byte stream, ~11 Hz during brew/backflush) ───────
bool           gicar_frame_ready();         // true once per Z-frame (clears on read)
const uint8_t* gicar_get_frame(int* len);   // raw Z-frame bytes
bool           gicar_z_shot_active();       // pair[14] == 0x10
float          gicar_z_temp();              // bytes[40..47] uint32 / 10.0 °C

// ── R-frame (poll response, 81 chars, ~760 ms interval) ─────────────────────
bool           gicar_r_frame_ready();       // true once per R-frame (clears on read)
float          gicar_r_temp();              // payload[28..29]/10.0 °C (live sensor)
bool           gicar_r_brew_active();       // payload[17] == 0x01
uint8_t        gicar_r_boiler_flags();      // payload[27] (use BF_* constants)
float          gicar_r_setpoint();          // setpoint from boot config read (°C)

// ── Commands ─────────────────────────────────────────────────────────────────
void gicar_write(uint16_t addr, const uint8_t* data, uint16_t len);
// Override the R-poll cadence (0 = restore the 760 ms default). The factory
// app polls only ~every 10 s during a backflush cycle.
void gicar_set_poll_period(uint32_t ms);
void gicar_read_req(uint16_t addr, uint16_t req_len);

// ── Diagnostics ──────────────────────────────────────────────────────────────
bool     gicar_handshake_ok();
uint32_t gicar_rx_total();
uint32_t gicar_frame_count();
uint32_t gicar_r_frame_count();
void     gicar_rx_state(bool* in_frame, int* rxlen);
void     gicar_debug_info(char* buf, int buflen);

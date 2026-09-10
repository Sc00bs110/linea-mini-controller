#pragma once
#include <Arduino.h>

// Per-shot brew-by-weight diagnostic record. A passive collector: ui.cpp is the
// single writer (it owns the bbw state machine), and mqtt.cpp drains the finished
// JSON on its 2 s tick — the same pending-flag handshake flow_log uses, and the
// only workable one here since publishes are suppressed during a brew.
//
// Exists because brew-by-weight failures are invisible after the fact: the record
// lands in HA history so a runaway or an early stop can be diagnosed from the shot
// that produced it rather than reproduced.

void bbw_event_shot_start(bool armed, float threshold_g);   // resets the collector
void bbw_event_arm_confirmed(uint32_t ms_into_shot, float w);
void bbw_event_stop(const char *reason, float w);           // "threshold"/"scale_lost"/"arm_failsafe"
void bbw_event_shot_end(uint32_t duration_ms, float final_weight_g,
                        uint8_t stop_attempts, uint32_t stop_latency_ms,
                        bool gave_up,
                        bool ack_seen, bool ack_ok,
                        uint8_t ack_resends);              // builds the JSON, sets pending

bool   bbw_event_has_pending();  // true once a finished shot's record is ready to publish
String bbw_event_take_json();    // returns pending JSON, clears the pending flag (call once)

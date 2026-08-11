#include "bbw_event.h"
#include "wlog.h"

// ─── Tunables ─────────────────────────────────────────────────────────────────

// An unarmed shot this short is a group-head flush or a lever bump, not a pull —
// no diagnostic value, and it would overwrite the retained record of a real shot.
// Armed shots are always recorded, however short: a stop at 2 s IS the bug.
static const uint32_t MIN_UNARMED_SHOT_MS = 5000;

// ─── In-memory record ─────────────────────────────────────────────────────────

static bool     s_armed        = false;
static float    s_threshold_g  = 0.0f;
static int32_t  s_arm_ms       = -1;      // ms into the shot when the arm gate confirmed
static float    s_arm_w        = 0.0f;
static String   s_stop_reason  = "none";  // "none" = no auto-stop (manual lever stop)
static float    s_stop_w       = 0.0f;
static bool     s_pending      = false;
static String   s_pending_json;

// ─── Public API ───────────────────────────────────────────────────────────────

void bbw_event_shot_start(bool armed, float threshold_g) {
    // Unconditional reset — including for shots that will be skipped at the end,
    // so nothing from the previous shot can leak into the next record.
    s_armed       = armed;
    s_threshold_g = threshold_g;
    s_arm_ms      = -1;
    s_arm_w       = 0.0f;
    s_stop_reason = "none";
    s_stop_w      = 0.0f;
}

void bbw_event_arm_confirmed(uint32_t ms_into_shot, float w) {
    s_arm_ms = (int32_t)ms_into_shot;
    s_arm_w  = w;
}

void bbw_event_stop(const char *reason, float w) {
    s_stop_reason = reason;
    s_stop_w      = w;
}

void bbw_event_shot_end(uint32_t duration_ms, float final_weight_g,
                        uint8_t stop_attempts, uint32_t stop_latency_ms,
                        bool gave_up,
                        bool ack_seen, bool ack_ok) {
    if (!s_armed && duration_ms < MIN_UNARMED_SHOT_MS) {
        wlogf("[bbw_event] skipped unarmed short shot (%ums)\n", (unsigned)duration_ms);
        return;
    }

    // "ok"/"ko" = the GICAR answered the stop's 0x0000 write; "none" = no ack
    // came back (or no auto-stop fired at all). Distinguishes a write the
    // controller never took from a command it took and ignored.
    const char *stop_ack = ack_seen ? (ack_ok ? "ok" : "ko") : "none";

    String json = "{";
    json += "\"duration_ms\":" + String((unsigned)duration_ms) + ",";
    json += "\"armed\":" + String(s_armed ? "true" : "false") + ",";
    json += "\"arm_ms\":" + String((long)s_arm_ms) + ",";
    json += "\"arm_w\":" + String(s_arm_w, 2) + ",";
    json += "\"stop_reason\":\"" + s_stop_reason + "\",";
    json += "\"stop_w\":" + String(s_stop_w, 1) + ",";
    json += "\"threshold_g\":" + String(s_threshold_g, 1) + ",";
    json += "\"stop_attempts\":" + String((unsigned)stop_attempts) + ",";
    json += "\"stop_latency_ms\":" + String((unsigned)stop_latency_ms) + ",";
    json += "\"gave_up\":" + String(gave_up ? "true" : "false") + ",";
    json += "\"stop_ack\":\"" + String(stop_ack) + "\",";
    json += "\"final_weight_g\":" + String(final_weight_g, 1);
    json += "}";

    s_pending_json = json;
    s_pending      = true;
    wlogf("[bbw_event] %ums armed=%d reason=%s stop_w=%.1f thr=%.1f "
          "attempts=%u latency=%ums gave_up=%d ack=%s final=%.1fg\n",
          (unsigned)duration_ms, s_armed, s_stop_reason.c_str(), s_stop_w,
          s_threshold_g, (unsigned)stop_attempts, (unsigned)stop_latency_ms,
          gave_up, stop_ack, final_weight_g);
}

bool bbw_event_has_pending() { return s_pending; }

String bbw_event_take_json() {
    s_pending = false;
    String json = s_pending_json;
    s_pending_json = "";
    return json;
}

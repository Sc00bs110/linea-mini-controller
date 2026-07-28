#include "flow_log.h"
#include "machine.h"
#include "scale.h"
#include "wlog.h"
#include <vector>

// ─── Tunables ─────────────────────────────────────────────────────────────────

static const uint32_t SAMPLE_INTERVAL_MS = 300;   // curve resolution
static const size_t   MAX_SAMPLES        = 150;   // caps the curve at ~45 s of shot
static const uint32_t MIN_SHOT_MS        = 10000; // discard flushes/lever-bumps under 10 s

// ─── In-memory recording state ────────────────────────────────────────────────

struct FSample {
    uint32_t t_ms;   // offset from shot start
    float    f;      // flow, g/s
    float    w;      // cup weight, g
};

static std::vector<FSample> s_samples;
static float    s_peak_flow      = 0.0f;
static uint32_t s_shot_start     = 0;
static uint32_t s_last_sample_ms = 0;
static int      s_decimate       = 1;
static uint32_t s_frame_count    = 0;
static bool     s_brewing        = false;
static bool     s_prev_brew      = false;
static bool     s_pending        = false;
static String   s_pending_json;

// ─── Shot lifecycle ───────────────────────────────────────────────────────────

static void begin_shot() {
    // Prefer the machine's own shot timestamp so t=0 lines up with the shot timer
    // the UI shows; fall back to now if the machine never stamped one.
    s_shot_start     = machine.shot_start_ms ? machine.shot_start_ms : millis();
    s_last_sample_ms = 0;
    s_peak_flow      = 0.0f;
    s_decimate       = 1;
    s_frame_count    = 0;
    s_samples.clear();
    s_samples.reserve(MAX_SAMPLES);
}

static void record_sample() {
    if (!scale_connected()) return;
    float flow = scale_flow();
    // Track the peak every loop, not just at sample points — a 300 ms grid would
    // alias away the brief spike that follows first drops.
    if (flow > s_peak_flow) s_peak_flow = flow;

    if (millis() - s_last_sample_ms < SAMPLE_INTERVAL_MS) return;
    s_last_sample_ms = millis();
    s_frame_count++;

    if (s_samples.size() >= MAX_SAMPLES) {
        s_decimate++;
        return;
    }
    if (s_decimate > 1 && (s_frame_count % s_decimate) != 0) return;

    uint32_t t = millis() - s_shot_start;
    s_samples.push_back({t, flow, scale_weight()});
}

// Serialise the finished shot into s_pending_json. Parallel arrays (t/f/w) rather
// than an array of objects: it roughly halves the payload, which matters against
// PubSubClient's fixed buffer.
static void build_pending_json() {
    uint32_t duration = millis() - s_shot_start;
    float final_weight = s_samples.empty() ? 0.0f : s_samples.back().w;
    float avg_flow = duration > 0 ? (final_weight / (duration / 1000.0f)) : 0.0f;

    String json = "{";
    json += "\"duration_ms\":" + String((unsigned)duration) + ",";
    json += "\"final_weight_g\":" + String(final_weight, 1) + ",";
    json += "\"peak_flow_gps\":" + String(s_peak_flow, 2) + ",";
    json += "\"avg_flow_gps\":" + String(avg_flow, 2) + ",";
    json += "\"n\":" + String((unsigned)s_samples.size()) + ",";

    json += "\"t\":[";
    for (size_t i = 0; i < s_samples.size(); i++) {
        if (i) json += ",";
        json += String((unsigned)s_samples[i].t_ms);
    }
    json += "],\"f\":[";
    for (size_t i = 0; i < s_samples.size(); i++) {
        if (i) json += ",";
        json += String(s_samples[i].f, 2);
    }
    json += "],\"w\":[";
    for (size_t i = 0; i < s_samples.size(); i++) {
        if (i) json += ",";
        json += String(s_samples[i].w, 1);
    }
    json += "]}";

    s_pending_json = json;
    s_pending = true;
    wlogf("[flow] shot ready: %ums, final=%.1fg, peak=%.2fg/s, avg=%.2fg/s, n=%u\n",
          (unsigned)duration, final_weight, s_peak_flow, avg_flow, (unsigned)s_samples.size());
}

// ─── Public API ───────────────────────────────────────────────────────────────

void flow_log_update() {
    s_brewing = machine.brew_active;

    if (s_brewing && !s_prev_brew) {
        begin_shot();
    } else if (s_brewing) {
        record_sample();
    } else if (!s_brewing && s_prev_brew) {
        uint32_t duration = millis() - s_shot_start;
        if (duration < MIN_SHOT_MS || s_samples.empty()) {
            wlogf("[flow] discarded short/scaleless shot (%ums)\n", (unsigned)duration);
        } else {
            build_pending_json();
        }
        s_samples.clear();
    }

    s_prev_brew = s_brewing;
}

bool flow_log_has_pending() { return s_pending; }

String flow_log_take_json() {
    s_pending = false;
    String json = s_pending_json;
    s_pending_json = "";
    return json;
}

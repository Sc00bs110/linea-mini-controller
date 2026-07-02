#include "shot_log.h"
#include "machine.h"
#include "wlog.h"
#include <LittleFS.h>
#include <vector>
#include <algorithm>

// ─── Tunables ─────────────────────────────────────────────────────────────────

static const char*    SHOT_DIR     = "/shots";
static const int      MAX_SHOTS    = 50;   // rolling window; oldest evicted past this
static const size_t   MAX_SAMPLES  = 200;  // ~18 s at 11 Hz Z-frames; decimate beyond
static const uint32_t MIN_SHOT_MS  = 1000; // ignore spurious sub-second brew blips

// ─── In-memory recording state ────────────────────────────────────────────────

struct Sample {
    uint32_t t_ms;   // offset from shot start
    float    temp;
};

static bool                 s_brewing      = false;
static bool                 s_prev_brew    = false;
static uint32_t             s_shot_start   = 0;
static uint32_t             s_last_sample  = 0;
static float                s_start_temp   = 0.0f;
static float                s_last_temp    = 0.0f;
static bool                 s_preinfusion  = false;
static int                  s_decimate     = 1;   // keep 1 of every N samples once full
static uint32_t             s_frame_count  = 0;   // raw frames seen this shot (for decimation)
static std::vector<Sample>  s_samples;
static int                  s_count        = 0;   // cached count of files in /shots

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Build the canonical path for a shot id: /shots/XXXXXXXXXX.json (10-digit, zero-padded).
static String shot_path(uint32_t id) {
    char name[32];
    snprintf(name, sizeof(name), "%s/%010u.json", SHOT_DIR, (unsigned)id);
    return String(name);
}

// Choose the live brew temperature source. Z-frames update at ~11 Hz during brew and
// carry the higher-resolution group reading; fall back to the R-frame boiler temp.
static float current_temp() {
    if (machine.z_temp_c > 0.0f) return machine.z_temp_c;
    return machine.coffee_temp_c;
}

// Count *.json files currently in /shots, refreshing the cached value.
static int scan_count() {
    int n = 0;
    File dir = LittleFS.open(SHOT_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) n++;
        }
    }
    s_count = n;
    return n;
}

// Collect shot filenames (just the basename, e.g. "0001234567.json"), sorted ascending.
// Filenames are zero-padded decimal ids, so lexical order matches chronological order.
static std::vector<String> sorted_shot_files() {
    std::vector<String> names;
    File dir = LittleFS.open(SHOT_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                String n = f.name();
                int slash = n.lastIndexOf('/');
                if (slash >= 0) n = n.substring(slash + 1);
                names.push_back(n);
            }
        }
    }
    std::sort(names.begin(), names.end(),
              [](const String& a, const String& b) { return a < b; });
    return names;
}

// Drop oldest files until strictly fewer than MAX_SHOTS remain, making room for one new shot.
static void evict_if_needed() {
    std::vector<String> names = sorted_shot_files();
    while ((int)names.size() >= MAX_SHOTS) {
        String victim = String(SHOT_DIR) + "/" + names.front();
        if (LittleFS.remove(victim)) {
            wlogf("[shot] evicted oldest %s\n", victim.c_str());
        } else {
            wlogf("[shot] evict failed: %s\n", victim.c_str());
            break;  // avoid infinite loop if removal keeps failing
        }
        names.erase(names.begin());
    }
}

// Serialise the just-finished shot to LittleFS as /shots/<id>.json.
static void write_shot(uint32_t id, uint32_t duration_ms) {
    evict_if_needed();

    String json = "{";
    json += "\"id\":" + String((unsigned)id) + ",";
    json += "\"duration_ms\":" + String((unsigned)duration_ms) + ",";
    json += "\"start_temp\":" + String(s_start_temp, 1) + ",";
    json += "\"end_temp\":" + String(s_last_temp, 1) + ",";
    json += "\"preinfusion\":" + String(s_preinfusion ? "true" : "false") + ",";
    json += "\"samples\":[";
    for (size_t i = 0; i < s_samples.size(); i++) {
        if (i) json += ",";
        json += "{\"t\":" + String((unsigned)s_samples[i].t_ms) +
                ",\"temp\":" + String(s_samples[i].temp, 1) + "}";
    }
    json += "]}";

    String path = shot_path(id);
    File f = LittleFS.open(path, "w");
    if (!f) {
        wlogf("[shot] open-for-write failed: %s\n", path.c_str());
        return;
    }
    size_t written = f.print(json);
    f.close();
    if (written != json.length()) {
        wlogf("[shot] short write %u/%u: %s\n",
              (unsigned)written, (unsigned)json.length(), path.c_str());
        return;
    }
    s_count++;
    wlogf("[shot] saved %s (%u ms, %u samples)\n",
          path.c_str(), (unsigned)duration_ms, (unsigned)s_samples.size());
}

// ─── Shot lifecycle ───────────────────────────────────────────────────────────

static void begin_shot() {
    s_shot_start  = machine.shot_start_ms ? machine.shot_start_ms : millis();
    s_last_sample = 0;
    s_start_temp  = current_temp();
    s_last_temp   = s_start_temp;
    s_preinfusion = machine.pump_active && !machine.brew_active;  // best-effort flag
    s_decimate    = 1;
    s_frame_count = 0;
    s_samples.clear();
    s_samples.reserve(MAX_SAMPLES);

    // Seed the first sample at t=0 so every shot has at least one point.
    s_samples.push_back({0, s_start_temp});
    wlogf("[shot] start id=%u temp=%.1f\n",
          (unsigned)s_shot_start, s_start_temp);
}

static void record_sample() {
    float temp = current_temp();
    s_last_temp = temp;
    s_frame_count++;

    // Once the buffer is full, switch to keeping every Nth frame so longer shots
    // still span the full duration instead of truncating at MAX_SAMPLES.
    if (s_samples.size() >= MAX_SAMPLES) {
        s_decimate++;  // progressively coarser; cheap and bounded
        return;
    }
    if (s_decimate > 1 && (s_frame_count % s_decimate) != 0) {
        return;
    }

    uint32_t t = millis() - s_shot_start;
    s_samples.push_back({t, temp});
}

static void end_shot() {
    uint32_t duration = millis() - s_shot_start;
    if (duration < MIN_SHOT_MS) {
        wlogf("[shot] discarded blip (%u ms)\n", (unsigned)duration);
        s_samples.clear();
        return;
    }
    write_shot(s_shot_start, duration);
    s_samples.clear();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void shot_log_init() {
    // LittleFS.begin(true) formats on first mount failure; harmless if already mounted.
    if (!LittleFS.begin(true)) {
        wlogf("[shot] LittleFS mount failed\n");
        return;
    }
    if (!LittleFS.exists(SHOT_DIR)) {
        LittleFS.mkdir(SHOT_DIR);
    }
    scan_count();
    wlogf("[shot] init: %d shots on disk\n", s_count);
}

void shot_log_update() {
    s_brewing = machine.brew_active;

    if (s_brewing && !s_prev_brew) {
        begin_shot();
    } else if (s_brewing) {
        // Sample once per loop while brewing; frame cadence is set upstream.
        record_sample();
    } else if (!s_brewing && s_prev_brew) {
        end_shot();
    }

    s_prev_brew = s_brewing;
}

String shot_log_list_json(int limit) {
    if (limit < 0) limit = 0;
    std::vector<String> names = sorted_shot_files();

    // Newest first: walk the sorted list from the end.
    String out = "[";
    int emitted = 0;
    for (int i = (int)names.size() - 1; i >= 0 && emitted < limit; i--) {
        String path = String(SHOT_DIR) + "/" + names[i];
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        String body = f.readString();
        f.close();

        // Extract the three summary fields without a JSON parser. The shot file is
        // written by us with a fixed field order, so light string scanning is safe.
        String id_s        = "0";
        String dur_s       = "0";
        String start_temp  = "0.0";

        int p;
        if ((p = body.indexOf("\"id\":")) >= 0) {
            int e = body.indexOf(',', p);
            if (e > p) id_s = body.substring(p + 5, e);
        }
        if ((p = body.indexOf("\"duration_ms\":")) >= 0) {
            int e = body.indexOf(',', p);
            if (e > p) dur_s = body.substring(p + 14, e);
        }
        if ((p = body.indexOf("\"start_temp\":")) >= 0) {
            int e = body.indexOf(',', p);
            if (e > p) start_temp = body.substring(p + 13, e);
        }

        if (emitted) out += ",";
        out += "{\"id\":" + id_s +
               ",\"duration_ms\":" + dur_s +
               ",\"start_temp\":" + start_temp + "}";
        emitted++;
    }
    out += "]";
    return out;
}

String shot_log_get_json(uint32_t shot_id) {
    String path = shot_path(shot_id);
    File f = LittleFS.open(path, "r");
    if (!f) return String("{}");
    String body = f.readString();
    f.close();
    return body;
}

int shot_log_count() {
    return s_count;
}

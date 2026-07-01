#include "gicar.h"
#include "wlog.h"
#include <SoftwareSerial.h>
#include <driver/gpio.h>

// ─────────────────────────────────────────────────────────────────────────────
// La Marzocco Linea Mini — Gicar protocol driver
//
// Wire: GPIO16=TX, GPIO17=RX ↔ Gicar CN11, 9600 8N1, invert=true.
// CN11 signals are idle-LOW 2.5 V LVCMOS, so both lines must be inverted.
//
// EspSoftwareSerial is used instead of hardware UART1 because the ESP32-C6
// HP UART shadow-register mechanism makes uart_set_line_inverse() ineffective.
// EspSoftwareSerial applies inversion at the GPIO/ISR level, bypassing the
// broken peripheral entirely.  At 9600 baud (104 µs/bit) software timing is
// well within ISR latency margins.
//
// All frames are ASCII printable characters. Every command and every machine
// frame ends in a 2-char uppercase-hex checksum = (sum of the ASCII values of
// every preceding char) mod 256. Payload bytes inside R/Z frames are carried as
// hex pairs (two ASCII chars per byte).
//
// Three machine→ESP32 frame types are recognised by their first char:
//   'R'  poll response   — 81 chars, authoritative machine state (~760 ms)
//   'Z'  shot telemetry  — 55 chars, autonomous, only during brew/backflush
//   'X'  X-probe reply    — 11 chars, boot handshake only
// ─────────────────────────────────────────────────────────────────────────────

// ── Framing constants ───────────────────────────────────────────────────────
static const int      R_FRAME_LEN   = GICAR_R_FRAME_LEN;  // 81
static const int      Z_FRAME_LEN   = GICAR_Z_FRAME_LEN;  // 54
static const int      X_FRAME_LEN   = 11;                 // "X00000001D9"
static const uint32_t POLL_PERIOD_MS = 100;               // R-poll cadence
static const uint32_t RX_TIMEOUT_MS  = 200;               // stale-buffer reset

// Poll command: read 35 bytes (0x0023) from address 0x4000 — live machine state block.
// Address 0x0000 (config) returns all-zero payload for runtime state fields.
static const char POLL_CMD[] = "R40000023DB";

// ── RX accumulator state ─────────────────────────────────────────────────────
static char     _rxbuf[GICAR_BUF_SIZE];
static int      _rxlen      = 0;
static bool     _in_frame   = false;   // true once a frame-start char is seen
static int      _expect_len = 0;       // target length for the current frame
static uint32_t _last_rx_ms = 0;       // last byte arrival (for RX timeout)

static uint32_t _rx_total      = 0;
static uint32_t _last_poll_ms  = 0;
static int      _init_xb       = -1;   // bytes received during X-probe (-1=not run)
static int      _init_rb       = -1;   // bytes received during config read (-1=not run)

// ── Z-frame (shot telemetry) results ─────────────────────────────────────────
static uint8_t  _z_frame[GICAR_BUF_SIZE];
static int      _z_frame_len   = 0;
static bool     _z_ready       = false;
static bool     _z_shot_active = false;
static float    _z_temp        = 0.0f;
static uint32_t _frame_count   = 0;    // Z-frame counter (legacy name)

// ── R-frame (poll response) results ──────────────────────────────────────────
static bool     _r_ready         = false;
static float    _r_temp          = 0.0f;
static bool     _r_brew          = false;
static uint8_t  _r_boiler        = 0;
static float    _r_setpoint      = 0.0f;   // from boot config read
static uint32_t _r_frame_count   = 0;
static uint32_t _last_r_frame_ms = 0;

// ── Handshake / boot state ───────────────────────────────────────────────────
static bool     _hs_ok = false;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Checksum: sum of the ASCII values of the first `len` chars, mod 256.
static uint8_t _cs(const char* s, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += (uint8_t)s[i];
    return (uint8_t)(sum & 0xFF);
}

// Decode one hex nibble; returns 0 for non-hex input (frames are validated by
// checksum, so a malformed nibble is caught upstream rather than here).
static uint8_t _nib(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0;
}

// Decode a hex byte pair (two ASCII chars) into a single byte.
static uint8_t _hex2(char hi, char lo) {
    return (uint8_t)((_nib(hi) << 4) | _nib(lo));
}

// Append a 2-char uppercase-hex checksum over the first `n` chars of `cmd`,
// then return the new total length. `cmd` must have room for 2 more chars.
static int _append_cs(char* cmd, int n, int cap) {
    uint8_t cs = _cs(cmd, n);
    n += snprintf(cmd + n, (size_t)(cap - n), "%02X", cs);
    return n;
}

// ── EspSoftwareSerial — bypasses broken C6 HP UART inversion ────────────────

static SoftwareSerial _sw_serial;

static void _uart_setup() {
    // Release IO_MUX bootloader lock on GPIO16/17 before SoftwareSerial claims them.
    gpio_reset_pin((gpio_num_t)GICAR_TX_PIN);
    gpio_reset_pin((gpio_num_t)GICAR_RX_PIN);
    // invert=true: CN11 is idle-LOW 2.5 V LVCMOS.
    _sw_serial.begin(GICAR_BAUD, SWSERIAL_8N1, GICAR_RX_PIN, GICAR_TX_PIN, true, 256);
}

static int _uart_available() {
    return _sw_serial.available();
}

static int _uart_read_byte() {
    return _sw_serial.read();
}

static void _uart_write(const uint8_t* data, int len) {
    _sw_serial.write(data, (size_t)len);
}

static void _uart_flush() {
    _sw_serial.flush();
}

// ── Frame builders ───────────────────────────────────────────────────────────
// Each builder writes a complete checksummed ASCII frame into `out` and returns
// its length. `out` must be at least GICAR_BUF_SIZE bytes.

static int _build_x_probe(char* out, int cap) {
    int n = snprintf(out, (size_t)cap, "X%04X%04X", 0x0000, 0x0000);
    return _append_cs(out, n, cap);
}

static int _build_poll(char* out, int cap) {
    int n = snprintf(out, (size_t)cap, "R%04X%04X", 0x0000, 0x001F);
    return _append_cs(out, n, cap);
}

static int _build_read(char* out, int cap, uint16_t addr, uint16_t req_len) {
    int n = snprintf(out, (size_t)cap, "R%04X%04X", addr, req_len);
    return _append_cs(out, n, cap);
}

static int _build_write(char* out, int cap,
                        uint16_t addr, const uint8_t* data, uint16_t dlen) {
    int n = snprintf(out, (size_t)cap, "W%04X%04X", addr, dlen);
    for (int i = 0; i < dlen && n < cap - 4; i++) {
        n += snprintf(out + n, (size_t)(cap - n), "%02X", data[i]);
    }
    return _append_cs(out, n, cap);
}

// ── Frame parsers ────────────────────────────────────────────────────────────

// R-frame: "R" + addr(4) + len(4) + 62 hex chars (31 payload bytes) + cs(2) = 73 chars.
// Address 0x0000, length 0x001F — matches R0000001FE9 poll command from original gateway.
//   payload[17]      brew active  (0x01 = brewing)
//   payload[27]      boiler flags bitmask
//   payload[28..29]  coffee temp, uint16 BE / 10.0 °C
static void _parse_r_frame(const char* buf, int len) {
    if (len != R_FRAME_LEN || buf[0] != 'R') {
        wlogf("[gicar] R-frame bad header len=%d c=%c\n", len, buf[0]);
        return;
    }

    // Checksum covers everything except the trailing 2 checksum chars.
    uint8_t want = _cs(buf, len - 2);
    uint8_t got  = _hex2(buf[len - 2], buf[len - 1]);
    if (want != got) {
        wlogf("[gicar] R-frame checksum %02X!=%02X\n", got, want);
        return;
    }

    // Payload bytes start at offset 9 (after 'R' + 4-char addr + 4-char len),
    // two ASCII hex chars per byte.
    uint8_t p17 = _hex2(buf[9 + 17 * 2], buf[9 + 17 * 2 + 1]);
    uint8_t p27 = _hex2(buf[9 + 27 * 2], buf[9 + 27 * 2 + 1]);
    uint8_t p28 = _hex2(buf[9 + 28 * 2], buf[9 + 28 * 2 + 1]);
    uint8_t p29 = _hex2(buf[9 + 29 * 2], buf[9 + 29 * 2 + 1]);

    _r_brew   = (p17 == 0x01);
    _r_boiler = p27;
    _r_temp   = (float)(((uint16_t)p28 << 8) | p29) / 10.0f;

    _r_ready = true;
    _r_frame_count++;
    _last_r_frame_ms = millis();

    // Throttle R-frame logging to ~1 Hz so the 760 ms poll cadence stays quiet.
    static uint32_t last_log_ms = 0;
    uint32_t now = millis();
    if (now - last_log_ms >= 1000) {
        last_log_ms = now;
        wlogf("[gicar] R temp=%.1f brew=%d boiler=%02X\n",
              _r_temp, _r_brew, _r_boiler);
    }
}

// Z6 stream frame: Z(1) + frame_len(1, = 0x36 = 54) + "0000"(4) + data(46) + cs(2) = 54 bytes.
//   bytes[40..47]  coffee temp — 8 ASCII hex chars, big-endian uint32 / 10.0 °C
//   bytes[48..51]  steam temp  — always "0000" (pressurestat only, no sensor)
//   bytes[28..37]  state bytes — raw values, brewing diff not yet decoded
static void _parse_z_frame(const char* buf, int len) {
    if (len != Z_FRAME_LEN || buf[0] != 'Z') {
        wlogf("[gicar] Z-frame bad header len=%d c=0x%02X\n", len, (uint8_t)buf[0]);
        return;
    }

    // Coffee temp: 8 ASCII hex chars at buf[40..47] → uint32 / 10.0 °C.
    uint32_t raw = ((uint32_t)_hex2(buf[40], buf[41]) << 24) |
                   ((uint32_t)_hex2(buf[42], buf[43]) << 16) |
                   ((uint32_t)_hex2(buf[44], buf[45]) <<  8) |
                    (uint32_t)_hex2(buf[46], buf[47]);
    _z_temp = (float)raw / 10.0f;

    // Shot detection: state bytes[28..37] diff not yet confirmed; keep previous logic.
    uint8_t p14 = _hex2(buf[6 + 14 * 2], buf[6 + 14 * 2 + 1]);
    bool shot = (p14 != 0x00);
    if (shot != _z_shot_active) {
        wlogf("[gicar] Z shot %s (p14=0x%02X)\n", shot ? "ACTIVE" : "stopped", p14);
    }
    _z_shot_active = shot;

    // Snapshot the raw frame for callers that want the bytes verbatim.
    _z_frame_len = (len < GICAR_BUF_SIZE) ? len : GICAR_BUF_SIZE;
    memcpy(_z_frame, buf, (size_t)_z_frame_len);

    _z_ready = true;
    _frame_count++;
}

// Map a frame-start char to its expected total length. Returns 0 if `c` is not
// a recognised frame-start char.
static int _frame_len_for(char c) {
    switch (c) {
        case 'R': return R_FRAME_LEN;
        case 'Z': return Z_FRAME_LEN;
        case 'X': return X_FRAME_LEN;
        default:  return 0;
    }
}

// Dispatch a fully-accumulated frame to the right parser.
static void _dispatch(const char* buf, int len) {
    switch (buf[0]) {
        case 'R': _parse_r_frame(buf, len); break;
        case 'Z': _parse_z_frame(buf, len); break;
        case 'X':
            // X-probe reply during boot — record handshake success.
            _hs_ok = true;
            break;
        default: break;
    }
}

// ── Blocking boot helper ─────────────────────────────────────────────────────
// Read raw bytes into `dst` until either `want_len` chars (starting with
// `start`) have been collected or `timeout_ms` elapses. Re-syncs on the
// `start` char so a mid-stream start does not strand us. Returns chars read.
static int _read_frame_blocking(char start, int want_len,
                                char* dst, int cap, uint32_t timeout_ms) {
    int n = 0;
    bool synced = false;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        while (_uart_available() && n < cap - 1) {
            int _b = _uart_read_byte(); if (_b < 0) break;
            char c = (char)_b;
            _rx_total++;
            if (c == start) { n = 0; synced = true; }   // (re)sync on start char
            if (!synced) continue;
            dst[n++] = c;
            if (n == want_len) { dst[n] = '\0'; return n; }
        }
    }
    dst[n] = '\0';
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void gicar_init() {
    _uart_setup();

    // Give the Gicar controller time to come up after power-on.
    delay(2000);

    char cmd[GICAR_BUF_SIZE];
    char resp[GICAR_BUF_SIZE];

    // 1) X-probe handshake. Expect an 11-char "X..." reply within 1 s.
    int n = _build_x_probe(cmd, sizeof(cmd));
    _uart_write((const uint8_t*)cmd, n);
    _uart_flush();
    wlogf("[gicar] boot: sent X-probe %.*s\n", n, cmd);

    int rn = _read_frame_blocking('X', X_FRAME_LEN, resp, sizeof(resp), 1000);
    _init_xb = rn;
    if (rn >= 1 && resp[0] == 'X') {
        _hs_ok = true;
        wlogf("[gicar] boot: X-probe OK (%s)\n", resp);
    } else {
        wlogf("[gicar] boot: X-probe timeout (%d bytes)\n", rn);
    }

    // 2) Config read: 32 bytes from 0x0000. Setpoint is payload[7..8],
    //    uint16 BE / 10.0 °C. Response is an R-frame: "R" + addr(4) + len(4) +
    //    2*32 hex chars + cs(2) = 75 chars.
    n = _build_read(cmd, sizeof(cmd), 0x0000, 0x0020);
    _uart_write((const uint8_t*)cmd, n);
    _uart_flush();
    wlogf("[gicar] boot: sent config read %.*s\n", n, cmd);

    const int CFG_LEN = 1 + 4 + 4 + 2 * 0x20 + 2;  // = 75
    rn = _read_frame_blocking('R', CFG_LEN, resp, sizeof(resp), 500);
    _init_rb = rn;
    if (rn == CFG_LEN && resp[0] == 'R') {
        uint8_t hi = _hex2(resp[9 + 7 * 2], resp[9 + 7 * 2 + 1]);
        uint8_t lo = _hex2(resp[9 + 8 * 2], resp[9 + 8 * 2 + 1]);
        _r_setpoint = (float)(((uint16_t)hi << 8) | lo) / 10.0f;
        wlogf("[gicar] boot: setpoint=%.1f\n", _r_setpoint);
    } else {
        wlogf("[gicar] boot: config read incomplete (%d bytes)\n", rn);
    }

    // 3) Arm the poll timer so the first poll fires immediately on entering
    //    gicar_process().
    _last_poll_ms = millis() - POLL_PERIOD_MS;
    _last_rx_ms   = millis();
}

void gicar_process() {
    uint32_t now = millis();

    // ── Poll cadence: send R40000023DB every POLL_PERIOD_MS ──────────────────
    if (now - _last_poll_ms >= POLL_PERIOD_MS) {
        _last_poll_ms = now;
        _uart_write((const uint8_t*)POLL_CMD, (int)(sizeof(POLL_CMD) - 1));
    }

    // ── Stale-buffer reset: a partial frame older than RX_TIMEOUT_MS is junk ──
    if (_in_frame && (now - _last_rx_ms) > RX_TIMEOUT_MS) {
        _rxlen      = 0;
        _in_frame   = false;
        _expect_len = 0;
    }

    // ── Raw hex dump of first 128 bytes received (one-shot debug) ───────────
    static bool _raw_dumped = false;
    static uint8_t _raw_buf[128];
    static int     _raw_len = 0;

    // ── Drain UART, accumulate frames, dispatch when complete ────────────────
    for (int _b; (_b = _uart_read_byte()) >= 0; ) {
        char c = (char)_b;
        _rx_total++;
        _last_rx_ms = now;

        if (!_raw_dumped && _raw_len < (int)sizeof(_raw_buf)) {
            _raw_buf[_raw_len++] = (uint8_t)c;
        }

        int flen = _frame_len_for(c);
        if (flen != 0) {
            // A frame-start char always begins a fresh frame, even mid-buffer.
            _rxlen      = 0;
            _in_frame   = true;
            _expect_len = flen;
        }

        if (!_in_frame) continue;

        if (_rxlen < GICAR_BUF_SIZE - 1) {
            _rxbuf[_rxlen++] = c;
        } else {
            // Overflow guard — should never hit for valid frames; resync.
            _rxlen      = 0;
            _in_frame   = false;
            _expect_len = 0;
            continue;
        }

        if (_rxlen == _expect_len) {
            _rxbuf[_rxlen] = '\0';
            _dispatch(_rxbuf, _rxlen);
            _rxlen      = 0;
            _in_frame   = false;
            _expect_len = 0;
        }
    }

    // ── Delayed diagnostics (start after 15 s so TCP client has time to connect) ──
    // Raw dump repeats every 30 s until frames successfully parse, so it can be
    // caught whenever a TCP log client connects during a shot.
    {
        static uint32_t s_dump_ms = 0;
        bool no_frames = (_frame_count == 0 && _r_frame_count == 0);
        if (no_frames && _raw_len == (int)sizeof(_raw_buf) &&
            millis() >= 15000 && millis() - s_dump_ms >= 30000) {
            s_dump_ms = millis();
            char line[280]; int n = 0;
            n += snprintf(line+n, sizeof(line)-n, "[gicar] raw[0..127]:");
            for (int i = 0; i < _raw_len && n < (int)sizeof(line)-5; i++)
                n += snprintf(line+n, sizeof(line)-n, " %02X", _raw_buf[i]);
            wlogf("%s\n", line);
        }
    }
    // Rate log every 5 s — includes z/r frame counts for frame-parsing diagnosis.
    {
        static uint32_t s_rate_ms = 0;
        static uint32_t s_rate_rx = 0;
        if (millis() >= 15000 && millis() - s_rate_ms >= 5000) {
            uint32_t d = _rx_total - s_rate_rx;
            wlogf("[gicar] rate rx=%lu d=%lu/5s (~%lu/s) fr_z=%lu fr_r=%lu\n",
                  _rx_total, d, d / 5, _frame_count, _r_frame_count);
            s_rate_rx = _rx_total;
            s_rate_ms = millis();
        }
    }

}

// ── Z-frame accessors ────────────────────────────────────────────────────────
bool gicar_frame_ready() {
    if (!_z_ready) return false;
    _z_ready = false;
    return true;
}

const uint8_t* gicar_get_frame(int* len) {
    *len = _z_frame_len;
    return _z_frame;
}

bool  gicar_z_shot_active() { return _z_shot_active; }
float gicar_z_temp()        { return _z_temp; }

// ── R-frame accessors ────────────────────────────────────────────────────────
bool gicar_r_frame_ready() {
    if (!_r_ready) return false;
    _r_ready = false;
    return true;
}

float   gicar_r_temp()         { return _r_temp; }
bool    gicar_r_brew_active()  { return _r_brew; }
uint8_t gicar_r_boiler_flags() { return _r_boiler; }
float   gicar_r_setpoint()     { return _r_setpoint; }

// ── Commands ─────────────────────────────────────────────────────────────────
void gicar_write(uint16_t addr, const uint8_t* data, uint16_t len) {
    char cmd[GICAR_BUF_SIZE];
    int n = _build_write(cmd, sizeof(cmd), addr, data, len);
    _uart_write((const uint8_t*)cmd, n);
}

void gicar_read_req(uint16_t addr, uint16_t req_len) {
    char cmd[GICAR_BUF_SIZE];
    int n = _build_read(cmd, sizeof(cmd), addr, req_len);
    _uart_write((const uint8_t*)cmd, n);
}

// ── Diagnostics ──────────────────────────────────────────────────────────────
bool     gicar_handshake_ok() { return _hs_ok; }
uint32_t gicar_rx_total()     { return _rx_total; }
uint32_t gicar_frame_count()  { return _frame_count; }
uint32_t gicar_r_frame_count(){ return _r_frame_count; }

void gicar_rx_state(bool* in_frame, int* rxlen) {
    *in_frame = _in_frame;
    *rxlen    = _rxlen;
}

void gicar_debug_info(char* buf, int buflen) {
    snprintf(buf, (size_t)buflen, "r:%lu z:%lu hs:%d xb:%d rb:%d",
             (unsigned long)_r_frame_count, (unsigned long)_frame_count,
             _hs_ok ? 1 : 0, _init_xb, _init_rb);
}

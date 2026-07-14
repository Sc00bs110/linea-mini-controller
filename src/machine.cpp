#include "machine.h"
#include "gicar.h"
#include "settings.h"
#include "wlog.h"
#include <Arduino.h>

// ── Machine state ──────────────────────────────────────────────────────────────
// Owned here; populated by machine_update() from gicar's parsed R/Z frames.
MachineState machine = {};

// Pending brew-stop wake: non-zero means send wake command at that millis() value.
static uint32_t s_brew_stop_wake_ms = 0;
// Set by machine_brew_stop_standby(); triggers a second wake after the lever
// returns to Stop (the machine queues the mid-brew standby until then).
static bool s_stop_wake_pending = false;

// Disconnect timeout: mark the machine offline if no R or Z frame arrives within
// this window. The R-frame poll runs at ~760 ms, so 3 s tolerates a few misses.
static const uint32_t DISCONNECT_TIMEOUT_MS = 3000;

// Temperature command bounds (register 0x0007). The Linea Mini app constrains the
// coffee setpoint to 88–96 °C; clamp here so a bad UI/NVS value never reaches the
// machine.
static const float TEMP_MIN_C = 88.0f;
static const float TEMP_MAX_C = 96.0f;

// ── Standby config-sync registers ──────────────────────────────────────────────
// After toggling standby (0x0000) the original gateway re-pushes these config
// bytes (all 0x00). Observed in capture 12_standby_toggle; mirrored here so the
// machine accepts the standby/wake transition cleanly.
static const uint16_t STANDBY_SYNC_REGS[] = {0x0400, 0x0401, 0x0402, 0x0406};

// ── Internal helpers ───────────────────────────────────────────────────────────

// Write a single byte to a Gicar register. Most commands are 1-byte payloads.
static void gicar_write_byte(uint16_t addr, uint8_t value) {
    gicar_write(addr, &value, 1);
}

// Standby-toggle brew stop: force standby (0x0000=0x00 + config-sync regs), then
// wake 500 ms later via the timer in machine_update(). Side effects observed in
// the field: the group's reheat and the next pull can need a lever cycle when the
// lever returns during the standby/wake window (a 10 s lever hold after the stop
// reheated fine, 2026-07-14). This is the only working stop — see machine_brew_stop().
static void machine_brew_stop_standby() {
    gicar_write_byte(0x0000, 0x00);
    for (uint16_t reg : STANDBY_SYNC_REGS) gicar_write_byte(reg, 0x00);
    s_brew_stop_wake_ms = millis() + 500;
    s_stop_wake_pending = true;   // second wake once the lever returns to Stop
    wlogf("[machine] brew_stop: standby sent, wake in 500ms\n");
}

// ── Public API ─────────────────────────────────────────────────────────────────

void machine_init() {
    // gicar_init() drives the full boot sequence (X-probe, identity read, config
    // block read) and starts the always-on R-frame poll. No H48 handshake or
    // heartbeat is required — the poll itself keeps the machine streaming.
    gicar_init();

    machine = {};

    // Until the machine reports its real setpoint via the boot config read, fall
    // back to the NVS-persisted coffee setpoint so the UI shows a sane value.
    machine.setpoint_c = settings.coffee_temp_c;
}

// Backflush sequence, matching capture 07 byte-count-exactly: the app sends
// 6 × W00E1=0x82 at 1 s intervals with normal polling, then goes NEAR-SILENT
// for the cycle — one R-poll every ~10 s, no further pulses — while the
// machine runs the multi-phase ~80 s cycle on its own. Chattering at the
// machine during the cycle (normal 760 ms polls, or extra 0x82 keep-alives)
// aborts it at the first pump-phase boundary — both were observed live
// 2026-07-03 before the capture's quiet period was understood.
static uint32_t s_clean_until_ms      = 0;
static uint32_t s_clean_next_pulse_ms = 0;
static int      s_clean_pulses_left   = 0;

void machine_update() {
    // Drain Serial1, run the poll cadence, and parse any R/Z frames.
    gicar_process();

    // ── Brew-stop wake: fires 500 ms after machine_brew_stop_standby() ──────────
    if (s_brew_stop_wake_ms != 0 && millis() >= s_brew_stop_wake_ms) {
        s_brew_stop_wake_ms = 0;
        gicar_write_byte(0x0000, 0x01);
        for (uint16_t reg : STANDBY_SYNC_REGS) gicar_write_byte(reg, 0x00);
        wlogf("[machine] brew_stop: wake sent\n");
    }

    // Debounce timer: brew_active only clears after 1 s of continuous false R-frames.
    // Absorbs single missed/corrupted polls caused by BLE scan coexistence interference.
    static uint32_t s_brew_false_ms = 0;

    // ── R-frame: authoritative machine state (~760 ms cadence) ──────────────────
    if (gicar_r_frame_ready()) {
        bool r_brew = gicar_r_brew_active();
        machine.coffee_temp_c   = gicar_r_temp();
        machine.boiler_flags    = gicar_r_boiler_flags();
        machine.steam_active    = (machine.boiler_flags & BF_STEAM_BOILER_ON) != 0;
        machine.heating_element = (machine.boiler_flags & BF_HEATING_ELEMENT) != 0;
        machine.pump_active     = (machine.boiler_flags & BF_PUMP_ACTIVE) != 0;
        machine.last_frame_ms   = millis();
        machine.connected       = true;

        if (r_brew) {
            machine.brew_active = true;
            s_brew_false_ms     = 0;
        } else if (machine.brew_active) {
            if (s_brew_false_ms == 0) s_brew_false_ms = millis();
            if (millis() - s_brew_false_ms > 1000) {
                machine.brew_active = false;
                s_brew_false_ms     = 0;
                // A standby-toggle stop sent mid-brew gets queued by the machine
                // while the lever holds it in brew: the pump cuts immediately but
                // the standby itself applies only once the lever returns to Stop
                // (field-observed 2026-07-14: machine dropped into standby at
                // lever return after every bbw stop). The mid-brew wake 500 ms
                // after the stop is ignored, so send a SECOND wake shortly after
                // the lever actually returns to undo the queued standby.
                if (s_stop_wake_pending) {
                    s_stop_wake_pending  = false;
                    s_brew_stop_wake_ms  = millis() + 500;
                    wlogf("[machine] brew_stop: lever returned — post-stop wake in 500ms\n");
                }
            }
        }

        // Throttle logging to ~1 Hz so the R-frame cadence doesn't flood the log.
        static uint32_t last_r_log_ms = 0;
        if (millis() - last_r_log_ms >= 1000) {
            last_r_log_ms = millis();
            wlogf("[machine] R temp=%.1f brew=%d flags=%02X steam=%d heat=%d pump=%d\n",
                  machine.coffee_temp_c, machine.brew_active, machine.boiler_flags,
                  machine.steam_active, machine.heating_element, machine.pump_active);
        }
    }

    // ── Z-frame: high-resolution shot telemetry (~11 Hz, only during brew) ───────
    // Z-frames give a faster temp/shot signal than the R-frame poll, but they only
    // appear while brewing, so they cannot drive disconnect detection on their own
    // (the R-frame poll handles the idle case). Also used to hold brew_active up
    // through single missed R-frame polls (BLE coexistence glitches).
    if (gicar_frame_ready()) {
        machine.z_shot_active = gicar_z_shot_active();
        machine.z_temp_c      = gicar_z_temp();
        machine.last_frame_ms = millis();
        machine.connected     = true;
        if (machine.z_shot_active) {
            machine.brew_active = true;
            s_brew_false_ms     = 0;
        }
    }

    // ── Clean cycle: 6-pulse burst, then radio silence until the window ends ──
    if (s_clean_until_ms != 0) {
        if (millis() >= s_clean_until_ms) {
            machine_clean_stop();
        } else if (s_clean_pulses_left > 0) {
            // A pulse landing on an already-running cycle aborts it after the
            // first pump phase (observed live: lever moved to Brew 2 s into the
            // burst → cycle died). If the brew starts early, drop the rest.
            if (machine.brew_active) {
                s_clean_pulses_left = 0;
                gicar_set_poll_period(10000);
                wlogf("[machine] clean: lever to Brew mid-burst — remaining pulses dropped\n");
            } else if (millis() >= s_clean_next_pulse_ms) {
                s_clean_next_pulse_ms = millis() + 1000;
                s_clean_pulses_left--;
                gicar_write_byte(0x00E1, 0x82);
                if (s_clean_pulses_left == 0) {
                    gicar_set_poll_period(10000);  // app-observed quiet period
                    wlogf("[machine] clean burst sent — ready for the lever\n");
                }
            }
        }
    }

    // ── Shot tracking ────────────────────────────────────────────────────────────
    // Use the R-frame brew flag as the primary edge source: it is present both
    // inside and outside a shot, whereas Z-frames vanish at idle.
    static bool prev_brew = false;
    if (!prev_brew && machine.brew_active) {
        machine.shot_start_ms = millis();
        wlogf("[machine] shot START\n");
    }
    if (prev_brew && !machine.brew_active) {
        wlogf("[machine] shot END duration=%.1fs\n",
              (millis() - machine.shot_start_ms) / 1000.0f);
    }
    prev_brew = machine.brew_active;

    // ── Setpoint tracking ────────────────────────────────────────────────────────
    // Re-read the 0x0000 config block every 60 s while the machine is idle so the
    // GICAR's REAL setpoint register stays visible (diagnosis: boiler ran 96-100°C
    // on 2026-07-04 with our target at 93 and no writes from us — needed to see
    // whether the machine itself was targeting high). Never send during a brew or
    // the cleaning cycle's quiet period: extra traffic aborts a running backflush.
    static uint32_t s_cfg_req_ms = 0;
    if (machine.connected && !machine.brew_active && !machine.z_shot_active &&
        !machine_clean_active() && millis() - s_cfg_req_ms >= 60000) {
        s_cfg_req_ms = millis();
        gicar_read_req(0x0000, 0x0020);
    }

    // Latch any sane value (>50 °C filters the zero/uninitialised state) so the UI
    // and MQTT track the machine's real setpoint instead of the NVS fallback.
    if (gicar_r_setpoint() > 50.0f && gicar_r_setpoint() != machine.setpoint_c) {
        machine.setpoint_c = gicar_r_setpoint();
        wlogf("[machine] setpoint now %.1f\n", machine.setpoint_c);
    }

    // ── Disconnect timeout ───────────────────────────────────────────────────────
    if (machine.connected && (millis() - machine.last_frame_ms) > DISCONNECT_TIMEOUT_MS) {
        machine.connected = false;
        wlogf("[machine] disconnected — no frames for %lu ms\n",
              (unsigned long)DISCONNECT_TIMEOUT_MS);
    }
}

// ── Commands ───────────────────────────────────────────────────────────────────

void machine_set_temp(float celsius) {
    celsius = constrain(celsius, TEMP_MIN_C, TEMP_MAX_C);

    // Register 0x0007 takes the setpoint as a big-endian uint16 of (°C * 10).
    int val = (int)(celsius * 10.0f + 0.5f);
    uint8_t data[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    gicar_write(0x0007, data, 2);
    wlogf("[machine] set_temp=%.1f (raw=%d)\n", celsius, val);
}

void machine_set_steam(bool on) {
    // Register 0x00E1: 0x81 = steam boiler on, 0x01 = off.
    gicar_write_byte(0x00E1, on ? 0x81 : 0x01);
    wlogf("[machine] set_steam=%d\n", on);
}

void machine_set_preinfusion(bool on) {
    // Register 0x000B: 0x01 = enable preinfusion, 0x00 = disable.
    gicar_write_byte(0x000B, on ? 0x01 : 0x00);
    wlogf("[machine] set_preinfusion=%d\n", on);
}

void machine_set_preinfusion_dur(uint8_t v) {
    // Register 0x00E2: preinfusion duration. App range 2–20; unit TBD.
    gicar_write_byte(0x00E2, v);
    wlogf("[machine] set_preinfusion_dur=%u\n", v);
}

void machine_clean_start() {
    s_clean_until_ms      = millis() + 135000;
    s_clean_pulses_left   = 6;
    s_clean_next_pulse_ms = 0;   // first pulse on the next update
    wlogf("[machine] clean cycle started (6-pulse burst, then quiet)\n");
}

void machine_clean_stop() {
    if (s_clean_until_ms != 0) wlogf("[machine] clean cycle stopped\n");
    s_clean_until_ms    = 0;
    s_clean_pulses_left = 0;
    gicar_set_poll_period(0);   // restore the normal 760 ms cadence
}

bool machine_clean_active() {
    return s_clean_until_ms != 0;
}

bool machine_clean_ready() {
    // Burst finished — safe to invite the lever.
    return s_clean_until_ms != 0 && s_clean_pulses_left == 0;
}

void machine_set_standby(bool standby) {
    // Verified sequence (capture 12_standby_toggle):
    //   1. 0x0000: 0x00 = enter standby, 0x01 = wake.
    //   2. 0x0400/0x0401/0x0402/0x0406: 0x00 config-sync push.
    //   3. On wake, re-assert the persisted steam state (the machine clears it on
    //      standby), otherwise steam silently stays off after waking.
    gicar_write_byte(0x0000, standby ? 0x00 : 0x01);

    for (uint16_t reg : STANDBY_SYNC_REGS) {
        gicar_write_byte(reg, 0x00);
    }

    if (!standby && settings.steam_on) {
        gicar_write_byte(0x00E1, 0x81);
    }

    machine.standby = standby;
    wlogf("[machine] set_standby=%d (steam_reassert=%d)\n",
          standby, (!standby && settings.steam_on));
}

// Brew stop: standby toggle, sent immediately. The 0x8000 "pump relay" write was
// tried first (2026-07-14) and confirmed live to do nothing — the GICAR rejects
// test-relay writes outside factory test mode (R-frames kept brew=1 pump=1 right
// through it), so every stop was really the fallback firing 3.5 s late, adding
// ~8-10 g of overshoot at test-shot flow rates. The lever machine has no factory
// remote-stop command; the standby toggle is the only working stop we have.
// machine.standby is NOT set so the UI stays in "active" state.
void machine_brew_stop() {
    machine_brew_stop_standby();
}

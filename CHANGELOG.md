# Changelog

Human-facing version history for the Linea Mini controller firmware. The
version string lives in `include/version.h` (`FW_VERSION`) and is bumped on
every deployed update; not every version gets a GitHub release — releases are
cut when a batch of field-tested changes is stable.

## v0.37 — 2026-07-17

Combined release covering the v0.31–v0.37 field updates.

### Added
- **Standby confirmation** — STANDBY pill at the top of the main screen with a
  hold-to-arm and yes/no confirm dialog, so standby can't be triggered by an
  accidental tap.
- **Scale Bluetooth on/off** — new "Scale BT" setting in the menu
  (NVS-persisted). Turning it off disconnects the scale and stops all BLE
  scanning.
- **SCALE status pill** — top-right of the main screen, showing scale
  connection state. Tapping it while disconnected triggers an immediate
  fast scan (and re-enables Scale BT if it was off), instead of waiting for
  the next idle scan cycle.
- **HEAT / STEAM status dots** — the old display-only HEAT and STEAM buttons
  are now two small round indicators at the top-left of the main screen.

### Changed
- **Main screen layout rework** — temperature reading moved to the centre of
  the screen, scale weight enlarged and placed between temperature and the
  clean counter, clean counter moved directly above the MENU button.
- **BLE idle scan interval 15 s → 60 s** — each scan actively runs for 10 s,
  so a 15 s pause meant ~40 % radio duty on the ESP32's shared 2.4 GHz
  radio, degrading WiFi/MQTT and making OTA transfers fail on marginal
  links. Fast-scan behaviour after a disconnect is unchanged.

### Fixed
- Brew-by-weight stop no longer has standby side effects; standby button
  responsiveness improved (v0.31).
- OTA tooling: `tools/ota_pull.py` now paces sends against the device's real
  consumption (small SO_SNDBUF + chunked writes) so a stalled transfer is
  reported instead of a false "served" success, and waits up to 10 minutes
  for the fetch; `tools/ota_upload.py` defaults its espota callback to port
  8070 to reuse the existing firewall rule.

## v0.30 — 2026-07-09
- GitHub-release OTA verified end-to-end: on-device check → confirm →
  install → reboot from a published release.

## v0.29
- GitHub update check frees the radio: BLE suspend/resume around the TLS
  fetch so the check completes reliably.

## v0.28 — 2026-07-09
- GitHub OTA dress-rehearsal release.

## v0.27
- Fixed GitHub install hang: manual streamed download replacing
  `httpUpdate.update()`, plus on-screen confirm/progress UI.

## v0.26
- Fixed GitHub OTA check (TLS buffers out of internal RAM) and CI-build
  credential seeding (CI binaries compile with blank secrets and never
  overwrite a device's stored WiFi/MQTT configuration).

## v0.25
- GitHub-release OTA: on-device update check and tagged-release CI pipeline
  (`.github/workflows/release.yml`).

## v0.24
- Reliable LAN OTA: MQTT-triggered HTTP-pull path plus hardened espota
  fallback.

## v0.23
- 20-second shot/flush rule (shorter pump runs count as flushes, not shots),
  cleaning-countdown reminder, and a clean-interval setting.

## v0.22
- Settings menu legibility and navigation buttons.

## v0.21
- Standby scheduling (daily wake/sleep, timezone-aware) and tap-to-wake
  overlay.

## v0.20
- HEAT indicator, MENU long-press affordance, slimmer settings menu.

## v0.18
- Machine-setpoint readback and machine-aware status dot.

## v0.17
- First working OTA updates; status line replaced by a connectivity dot.

## v0.16
- UI rework: edge adjusters, CLEAN on the main screen, bold type,
  brew-by-weight auto-offset. Guided backflush cleaning cycle with verified
  GICAR choreography.

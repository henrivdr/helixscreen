<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Slow-boot splash UX

**Branch:** `feature/slow-boot-splash-ux` · **Started:** 2026-06-13

## Problem

On slow devices (Creality K2, Flashforge AD5M Forge-X), cold boot runs a
launcher/init-script Moonraker gate (`platform_wait_for_services`, up to 120s)
*before* `helix-screen` launches. The init script pre-starts `helix-splash`,
but the splash hard-exits at `MAX_LIFETIME_SEC = 30` and only resumes/hands-off
when the **main app** sends `SIGUSR1` — which can't happen until the gate
clears. Result on any boot where Moonraker takes >30s:

> splash (0–30s) → **blank screen** → UI

Confirmed from code: `src/helix_splash.cpp:508-526` (30s loop cap),
`assets/config/platform/hooks-k2.sh:36-67` (gate), the init script pre-starts
the splash. Secondary issues: the splash spins a 60fps loop on a static image
(~60% CPU); a warm restart with Moonraker already up still sat ~90s in the gate
(probe not detecting a ready server promptly — needs investigation).

## Scope

| Change | Files | Platforms |
|--------|-------|-----------|
| #1 Splash lifetime driven by gate heartbeat (no blank gap) + lower CPU | `src/helix_splash.cpp` | All (helps wherever gate > 30s) |
| #2 Status text on splash ("Starting Klipper… 40s") | `src/helix_splash.cpp` + gate writers | K2, AD5M-forgex (gate writes status) |
| #3 Robustify Moonraker probe + log first-success latency | `hooks-k2.sh`, `hooks-ad5m-forgex.sh` | K2, AD5M-forgex |

AD5X shares `hooks-ad5m-zmod.sh` (10s `/dev/fb0` wait, **no Moonraker gate**) —
gets #1's lifetime/CPU hardening for free; #2/#3 don't apply. Other gate-less
platforms (Pi, K1, CC1, etc.) keep current 30s behavior unchanged (no heartbeat
file → fall back to default cap).

## Design

### Status-file protocol (shared by #1 + #2)
- Path: `${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}` (tmpfs; both
  init script and splash run as root).
- Content: the human message to display (one line). File **mtime = heartbeat.**
- Gate rewrites it each poll iteration → updates mtime + message.
- Absence (gate-less platforms) → splash behaves exactly as today.

### #1 lifetime (helix_splash.cpp)
- Default deadline = start + 30s (unchanged when no heartbeat).
- Each loop: if status mtime is fresh (`now - mtime <= HEARTBEAT_STALE_SEC`,
  ~5s), push deadline to `now + HEARTBEAT_STALE_SEC + grace`, capped at
  `start + ABSOLUTE_MAX_SEC` (180s, covers 120s gate + ~8s app start).
- `SIGUSR1`/`SIGTERM` handoff unchanged.
- CPU: on FBDEV, lengthen loop period (status updates ~2/s are plenty); keep
  signal responsiveness (usleep is interrupted by signals).

### #2 status label (helix_splash.cpp)
- Add a themed label below the logo; set text from status-file contents each
  loop; hide when empty/absent.

### #3 gate (hooks)
- **K2** (`python3` available, no curl): replace per-second `python3` *spawn*
  with a single long-lived `python3` poller that loops internally (1s cadence,
  2s per-request timeout), writes the status file each iteration, logs the
  first-success latency, returns 0 on ready. Fixes spawn churn + tolerates load.
- **AD5M-forgex** (BusyBox `wget`, python3 not guaranteed): keep the shell
  while-loop + `wget`, add status-file write per iteration + first-success
  latency log.
- Both: same status-file format the splash reads.

## Tests (test-first)
1. Catch2: `evaluate_splash_status(contents, mtime, now, start)` → `{alive,
   message}` decision table (fresh heartbeat extends; stale falls to 30s; absent
   = default). Native-buildable, no fbdev.
2. Bats: source each hook, run `platform_wait_for_services` against a mock HTTP
   server (python `http.server`); assert fast return when ready, status-file
   written, timeout path logs the warning.

## Status
- [x] Plan written
- [x] #1/#2 splash helper (`include/splash_status.h`) + Catch2 test (`tests/unit/test_splash_status.cpp`, 6 cases green)
- [x] #1/#2 splash loop integration (`src/helix_splash.cpp`: heartbeat-driven lifetime, status label, 10 FPS idle on fbdev)
- [x] #3 K2 gate (`hooks-k2.sh`: single python3 poller, monotonic, status writes, latency log, URL/timeout env overrides)
- [x] #3 AD5M-forgex gate (`hooks-ad5m-forgex.sh`: status writes, 2s wget timeout)
- [x] Bats gate test (`test_platform_hooks.bats`: added k2 contract/shellcheck/syntax + 2 behavioral gate tests; 40/40 green)
- [x] Native build + tests green (Catch2 [splash] + bats hooks)
- [x] Cross-build K2 (helix_splash.cpp compiles clean; fixed a real %ld/64-bit time_t varargs bug)
- [x] Deploy K2 + cold reboot validated: splash alive through a 37s gate showing "Starting Klipper… Ns" → "Starting HelixScreen…"; `Moonraker ready after 37s` latency log; clean SIGUSR1 handoff; no bail msgs; MemAvailable ~360 MB (valve never near tripping). Panel-pixel confirmation pending user glance on next reboot.
- [ ] Review + commit

## Notes
- Status-file path: `${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}`. tmpfs on K2 (cleared on reboot). On a warm helix restart an old file may linger <1s until the gate overwrites — harmless (still a valid heartbeat; gate writes immediately).
- AD5X shares `hooks-ad5m-zmod.sh` (10s fb wait, no Moonraker gate) → gets the splash lifetime/CPU hardening for free; #2/#3 N/A.
- The "warm restart waited ~90s" anomaly: K2 gate now logs `Moonraker ready after Ns` so the next device boot reveals the true detection latency. Single persistent python proc + immediate first poll + 2s timeout should detect an already-up server within ~1s.

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
- [x] Deploy K2 + cold reboot validated (mechanism): boot-play killed, single disp layer, splash alive through the gate AND through helix-screen startup (no blank gap), status pill in the upper-right corner (fb capture). `Moonraker ready after Ns` latency log working.
- [x] Commit (feature branch; not merged to main — Preston has parallel main work)

## SESSION STATE — PAUSED 2026-06-13 (resume here)

### What this turned into
Started as "splash shows blank during the slow Moonraker gate." The blank-gate
fixes (lifetime/status/CPU/valve + gate robustify) are done. But the REAL K2
visibility bug was **boot-play** (see section below) — the splash was rendering
to fb0 the whole time but hidden under Creality's z5 video overlay. Killing
boot-play is the single highest-impact fix.

### Done + validated on K2 hardware (192.168.1.74, root/creality_2024)
- **`killall boot-play`** in `platform_stop_competing_uis` (hooks-k2.sh) — removes the z5 Creality overlay so our splash/UI are actually visible. Verified via `/sys/class/disp/disp/attr/sys` (2 layers → 1) and a live kill.
- **Blank-gap fix** (`splash_should_continue` in `include/splash_status.h`): once a heartbeat is seen, stay alive until helix-screen's SIGUSR1 (or 180s backstop) — do NOT fall back to the 30s cap. helix-screen suppresses its own paint until the splash exits, so exiting early (old bug, at gate-end) blanked the screen for ~20s. Verified: splash + helix-screen co-own fb0 during the ~28s UI startup, no blank.
- **Status label** moved to `LV_ALIGN_TOP_RIGHT` (was BOTTOM_MID, over the logo). User's position words map directly to LVGL: bottom-center=BOTTOM_MID, version=BOTTOM_RIGHT, "upper right"=TOP_RIGHT. Subtle pill bg shown only when text present.
- **Paint speed**: reverted my 10 FPS fbdev throttle (it spaced the PARTIAL-render 60-line stripes 100ms apart → visible line-by-line wipe) back to 60 FPS; added one synchronous `lv_refr_now(display)` before the loop; self-heal 1s→3s.

### OPEN — needs user's eyes on next reboot (could not verify remotely)
1. **Is the count actually in the upper-right, clear of the logo?** (fb capture is 180° rotated from the physical panel, so capture orientation is unreliable for placement — trust the user, not the capture.)
2. **Is the logo paint still line-by-line?** If YES after the throttle revert, the cause is the **compressed** splash image decode on the slow K2 (`splash-3d-dark-small.bin` = 530 KB for 384k px → compressed). FIX: give the splash a **full-screen draw buffer + RENDER_MODE_FULL** (override after `lv_linux_fbdev_create`) so it decodes once and flushes the whole screen in one shot, OR ship an uncompressed `.bin` for K2. The global `LV_LINUX_FBDEV_RENDER_MODE` is PARTIAL/60-line/double-buffer (lv_conf.h:1103) — do NOT change globally; do it splash-only. Check `lv_linux_fbdev.c` flush_cb handles a full-screen area (it should).

### Deployment / repo state
- Manually deployed to the K2 device: `bin/helix-splash` + `platform/hooks.sh` (NOT a release; not in the installer/tarball yet). Boot flow: `S99helixscreen` (real init) pre-starts splash → sources `platform/hooks.sh` `platform_wait_for_services` (the gate) → launcher → helix-screen.
- K2 boot is slow (this session: 37–54s Moonraker gate + ~28s discovery = home panel at ~uptime 70–91s). That total is Moonraker/Klipper cold-init, not us; the splash now covers all of it.
- Build: `make k2-docker` (incremental, ccache). Deploy splash: scp to `/mnt/UDISK/helixscreen/bin/helix-splash`; hook to `/mnt/UDISK/helixscreen/platform/hooks.sh`. No `make deploy-k2` used (it deploys everything + uses key-auth ssh; device is password creality_2024 → use sshpass scp -O).
- Branch `feature/slow-boot-splash-ux`. Not merged. The `killall boot-play` fix is K2-specific and should also be considered for the installer so it ships to users (it's only in the runtime hook now).

## boot-play overlay (the real K2 visibility bug — found 2026-06-13 via on-device layer dump)
The splash/UI rendered correctly to `/dev/fb0` but was **invisible on the panel**:
Creality's boot animation `/sbin/boot-play` (init `S01play`) does NOT draw to fb0 —
it puts a **YUV video layer on the Allwinner display engine at z-order 5**,
composited ON TOP of the fbdev UI layer (z0). `/sys/class/disp/disp/attr/sys`
during the gate showed two layers (`ch0 z5 fmt72` over `ch1 z0 fmt0`); a live
`killall boot-play` removed the z5 layer, leaving only z0 (our UI). `desc.txt`
loops part1 forever (`p 0 0 part1`) and nothing stops it (we replaced the stock
UI that signals `bootanimation_exit`), so it covered our splash AND the UI for
the whole ~70s boot. Fix: `killall boot-play` in `platform_stop_competing_uis`
(SIGTERM → clean `de_disp_uninit`; procd does not respawn it). K2-only (Creality
mechanism); AD5M/AD5X boot animations differ.

## Notes
- Status-file path: `${HELIX_SPLASH_STATUS_FILE:-/tmp/helix-splash-status}`. tmpfs on K2 (cleared on reboot). On a warm helix restart an old file may linger <1s until the gate overwrites — harmless (still a valid heartbeat; gate writes immediately).
- AD5X shares `hooks-ad5m-zmod.sh` (10s fb wait, no Moonraker gate) → gets the splash lifetime/CPU hardening for free; #2/#3 N/A.
- The "warm restart waited ~90s" anomaly: K2 gate now logs `Moonraker ready after Ns` so the next device boot reveals the true detection latency. Single persistent python proc + immediate first poll + 2s timeout should detect an already-up server within ~1s.

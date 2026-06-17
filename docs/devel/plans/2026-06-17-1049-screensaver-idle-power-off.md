# #1049 — Screensaver-as-idle + real display power-off (fbdev/DRM HDMI)

Branch: `fix/1049-screensaver-idle-power-off`. Evidence: bundle `3VDWMLYQ` (Pi 4, `pi32`/fbdev,
`sleep_sec=60`, `dim_sec=600`, `screensaver_type=0`, `[Backlight-None] ... no hardware`).

## The two existing timeouts ARE the two stages (corrected — the transcript was wrong)

`dim_sec` is NOT hidden. Both are user-facing dropdowns, both measured from the same idle clock
(`lv_display_get_inactive_time`), MacOS/KlipperScreen-style:

| Label | Key | Default | Role |
|---|---|---|---|
| **Screen Dim** | `/display/dim_sec` | 600s | dim brightness OR show screensaver ("instead of dimming") — intermediate idle stage |
| **Display Sleep** | `/display/sleep_sec` | 1200s | screen sleeps — the **final / power-off** stage |

Coupling rule (already implemented, but see Bug 1): **Display Sleep ≥ Screen Dim** — power-off can
never precede the screensaver/dim stage. Enforced in `DisplaySettingsManager`:
`set_display_sleep_sec()` clamps sleep up to dim (~517), `set_display_dim_sec()` bumps sleep up
(~489), init cross-validation (~209).

## Two bugs, verified from the bundle

**Bug 1 — screensaver unreachable on no-backlight devices because the Sleep≥Dim coupling is
SKIPPED there.** The coupling only runs when `has_dimming_control()` is true. The reporter's Pi has
no dimmable backlight (`Backlight-None`), so coupling was skipped → they set Sleep=60s < Dim=600s →
in `check_display_sleep()` the sleep branch (display_manager.cpp:826) fires at 60s and blanks
before the dim/screensaver branch (:829) at 600s ever runs. The screensaver is starved. ("Test
Screensaver" works only because `preview_screensaver()` (:947) bypasses gating.)

**Bug 2 — no real power-off for fbdev/DRM HDMI.** No backlight device → `m_use_hardware_blank`
false → `enter_sleep()` (:644) paints a software black overlay (`create_sleep_overlay()`, :684).
No DPMS / `FBIOBLANK FB_BLANK_POWERDOWN` / `vcgencmd display_power` anywhere. Panel stays powered.

## Design — two-stage, both timeouts from idle (MacOS/KlipperScreen)

The two-stage flow ALREADY works on dimming-capable devices (Dim→screensaver/dim, Sleep→sleep).
The fix makes it work on no-backlight fbdev/DRM devices and makes "sleep" a real power-off.

### Part 1 — Bug 1: protect the screensaver stage on no-backlight devices
Extend the Sleep≥Dim coupling so it fires when **`has_dimming_control()` OR a screensaver is
enabled** (today: dimming-only). On a no-backlight device the screensaver IS the dim-stage visual,
so Sleep must still be forced ≥ Dim. Apply at all four sites in `DisplaySettingsManager`:
- `set_display_sleep_sec()` (~517) — clamp sleep up to dim,
- `set_display_dim_sec()` (~489) — bump sleep up to dim,
- init cross-validation (~209) — so the reporter's already-persisted `sleep=60 < dim=600`
  self-corrects on next launch (this is what un-bricks the field config),
- guard helper: replace the `has_dimming_control()` gate with `has_dimming_control() || screensaver_enabled()`.

With Dim < Sleep enforced, `check_display_sleep()` needs no structural change: the dim/screensaver
branch (:829) fires at Dim, the sleep branch (:826) at Sleep. Verify the `(can_dim || has_screensaver)`
guard on :829-830 already lets the screensaver run on no-backlight devices (it does). Add a test that
the awake path reaches the screensaver at Dim when there's no backlight.

### Part 2 — Bug 2: real capability-gated power-off at the Sleep stage
`enter_sleep()` (:644) already stops a running screensaver (:647-650). Add a real power-off used when
`!m_use_hardware_blank` instead of (or before) the software overlay:
- fbdev: `ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN)`
- DRM: connector/CRTC DPMS off
- Pi fallback: `vcgencmd display_power 0` / `1`
- else: existing software overlay (unchanged last-resort fallback).
Mirror power-on in `wake_display()` (:862) **before** `lv_refr_now` (honor the #303 wake-race note).
Track which path was used (powered-off vs overlay) so wake restores the correct one.

Net result = KlipperScreen two-stage: idle → (Dim) screensaver/dim → (Sleep) panel powers off; touch
wakes and powers back on. Both timeouts counted from idle.

## Capability detection (Bug 2)

Add a `supports_power_off()` probe + a `power_off()/power_on()` path. Investigate the existing
backend abstraction (`m_backend->blank_display()/unblank_display()`) and the backlight classes
(`Backlight-None`, `supports_hardware_blank`) for the right seam — prefer extending the display
backend, gated so the software overlay remains the fallback when no power-off mechanism exists.
The `#303` race note in `enter_sleep()` (FBIOBLANK on wake left black screens) applies — the new
power-off path must restore + `lv_refr_now()` on wake exactly like the existing hardware-blank path.

## Interactions to preserve (regression surface — write tests for these)

- Print inhibit (`inhibit_sleep_entry`, sleep-while-printing=false) — must still block idle entry.
- Wake gating / `disable_input_briefly()` on wake-from-sleep.
- Preview grace window (`m_screensaver_is_preview`, 750ms) — preview must not auto-dismiss.
- Auto-lock on wake (`was_sleeping || was_dimmed`, not on preview).
- `NavigationManager::suspend_active()/resume_active()` around screensaver start/stop.
- Sleep callbacks (`m_sleep_callbacks`, camera suspend/resume) fire on both new paths.
- `HELIX_ENABLE_SCREENSAVER` ifdef guards.

## Tests (test-first, real failures)

- check_display_sleep with screensaver ON + `sleep < dim`: asserts screensaver starts at sleep
  timeout (currently blanks → must fail before fix).
- screensaver OFF: asserts `power_off()` invoked (mock backend) at sleep timeout, not just overlay.
- capability probe false → falls back to software overlay.
- wake restores power + brightness on both paths; auto-lock + preview-grace unchanged.

## Status / deferred / risk (2026-06-17)

**Implemented + reviewed-clean + tests green:** Part 1 (coupling) fully. Part 2 power-off seam
is **fbdev-only** (`FBIOBLANK FB_BLANK_POWERDOWN`). DRM DPMS and the `vcgencmd display_power`
fallback are **NOT implemented** — DRM/SDL backends inherit the no-op default and fall back to the
software overlay. The seam (`DisplayBackend::supports_power_off()/power_off()/power_on()`) is in
place so DRM DPMS can be added later without touching DisplayManager.

**OPEN HARDWARE RISK (unverified):** modern Pi uses the vc4-KMS/DRM driver; `/dev/fb0` is DRM
**fbdev emulation**. `FBIOBLANK FB_BLANK_POWERDOWN` on the emulated fbdev may NOT actually DPMS-off
an HDMI panel — the real power-off on a KMS Pi typically needs DRM connector DPMS or
`vcgencmd display_power 0`. So the fbdev path may be insufficient on exactly the reporter's device
(Pi 4 + HDMI). **Must hardware-verify on a Pi with an HDMI panel before claiming Bug 2 fixed**; if
FBIOBLANK doesn't cut the panel, add the `vcgencmd display_power` fallback (reliable on Pi) and/or
DRM DPMS. Bug 1 (coupling/screensaver) is not subject to this risk.

## Process
- `git rev-parse --abbrev-ref HEAD` == `fix/1049-screensaver-idle-power-off` before EVERY commit.
- Threading per CLAUDE.md (AsyncLifetimeGuard, no sync widget deletes in queued cbs).
- Build: `make -j`; tests `make test-run`. Hardware-verify on a Pi w/ HDMI panel.

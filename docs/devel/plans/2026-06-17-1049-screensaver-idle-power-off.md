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
covers fbdev (`FBIOBLANK FB_BLANK_POWERDOWN`) AND now **DRM connector DPMS** (the real path for
HDMI/no-backlight devices — see Phase 0 below).

### Phase 0 — DRM master + fd investigation (the crux: CASE A)

Findings (verbatim file:line):
- **Backend on pi32 / CB1:** auto-detect tries **DRM first** (`DisplayBackend::create_auto()`,
  `src/api/display_backend.cpp:199-296`); `DisplayBackendDRM::is_available()` requires a connected
  connector (`display_backend_drm.cpp:91-122`). On a Pi 4 / CB1 with HDMI connected, **DRM wins**;
  fbdev is only the fallback. So the reporter's "software overlay" came from `m_use_hardware_blank=false`
  with no power-off seam — DRM was the renderer but had no power_off().
- **helix IS DRM master (firmly).** Both LVGL render paths own the card fd and do KMS modesetting
  (`drmModeSetCrtc`), which requires master:
  - **EGL path** (pi32/CB1 default, `HELIX_ENABLE_OPENGLES` → `LV_LINUX_DRM_USE_EGL=1`):
    `lv_linux_drm_egl.c:486 drm_device_init()` opens the fd (`open(path,O_RDWR)`, :492) and calls
    `drmSetMaster(ctx->fd)` (:535). driver_data is `lv_drm_ctx_t*`, fd is its first member (`int fd;` :39).
  - **non-EGL path:** `lv_linux_drm.c drm_setup()` opens the fd; `patches/lvgl-drm-set-master.patch`
    adds the `drmSetMaster()` retry loop. driver_data is `drm_dev_t*`, fd is its first member (`int fd;` :60).
  → In-process `drmModeConnectorSetProperty(fd, conn, DPMS, OFF)` on **that** fd is permitted; no
  second (non-master) fd, no EPERM. **CASE A is correct.**
- **No public getter for the fd.** Added `lv_linux_drm_get_fd(disp)` via patches mirroring
  `patches/lvgl-drm-egl-getters.patch` — one impl in each of `lv_linux_drm.c` (non-EGL) and
  `lv_linux_drm_egl.c` (EGL), declared once in `lv_linux_drm.h` under `#if LV_USE_LINUX_DRM`.
- **libdrm already linked** for DRM builds (`-ldrm`, `Makefile:660`; `-I/usr/include/libdrm`,
  `mk/cross.mk`). `xf86drm.h`/`xf86drmMode.h` already included by `display_backend_drm.cpp`.

### CASE A implementation
`DisplayBackendDRM::supports_power_off()/power_off()/power_on()` use the master fd from
`lv_linux_drm_get_fd(display_)`: enumerate connectors, find the CONNECTED one, locate its "DPMS"
property, `drmModeConnectorSetProperty(... DRM_MODE_DPMS_OFF/ON)`. `power_on()` runs before
`lv_refr_now` on wake (#303). The **fbdev** `FBIOBLANK FB_BLANK_POWERDOWN` path (committed at
044db95af) stays for genuine fbdev devices (AD5M/AD5X) and is UNAFFECTED.

**Verification target (CB1 .112):** Allwinner, HDMI, `/sys/class/backlight/` EMPTY (reproduces the
no-backlight condition), `card0-HDMI-A-1: connected`, NO vcgencmd. DRM DPMS is the ONLY real
power-off there. Verify by reading `/sys/class/drm/card0-HDMI-A-1/dpms` == `Off` after sleep, `On`
after wake.

**vcgencmd / firmware path: deliberately NOT added.** It is VideoCore/Pi-only (CB1 has no vcgencmd),
the in-process mailbox "blank screen" tag only blanks the framebuffer (not HDMI DPMS-off — same
limitation as FBIOBLANK), and DRM DPMS already covers the real devices we can verify. Documenting
this rather than shipping a path that pretends to power off.

Bug 1 (coupling/screensaver) is not subject to any of this hardware risk.

### Power-off gate is LAST RESORT (Snapmaker U1 regression, hardware-confirmed)
The `m_use_power_off` gate must require **neither a hardware blank NOR a usable backlight** — not
just "no hardware blank". The U1 has a working Sysfs pwm-backlight (`set_brightness(0)` cleanly turns
it off) yet reports `Hardware blank: false` and exposes a DPMS-capable DRM connector. With the
original gate (`!m_use_hardware_blank`), `m_use_power_off` became true and DRM DPMS-off disabled the
Rockchip VOP2 CRTC → panel **permanently black**; wake's DPMS-on does not recover it (see
`assets/config/platform/hooks-snapmaker-u1.sh` "DRM CRTC keepalive"). Fix: pure helper
`DisplayManager::should_use_power_off(use_hw_blank, has_usable_backlight, backend_supports_power_off)`
= `!use_hw_blank && !has_usable_backlight && backend_supports_power_off`. Any device with a backlight
turns the backlight off instead (the existing `set_brightness(0)` in `enter_sleep()` always runs).
Verified: Backlight-None+DPMS (reporter/CB1) → still power-off; usable backlight (U1) → no power-off;
hardware-blank (AD5M) → unchanged.

**Implementation status (uncommitted, on top of 044db95af):** LVGL `lv_linux_drm_get_fd()` getter
added via amended `patches/lvgl-drm-flush-rotation.patch` (`.h` decl + non-EGL `.c` impl) and
`patches/lvgl-drm-egl-getters.patch` (EGL `.c` impl); patches re-apply cleanly from pristine in
build order. `DisplayBackendDRM::supports_power_off()/power_off()/power_on()` + `set_connector_dpms()`
implemented. DisplayManager needed NO change — its power-off path is backend-agnostic. Native build
(SDL) + tests build clean; DRM TU verified with `-fsyntax-only -Wall -Wextra` against libdrm.
`./build/bin/helix-tests "[1049]"` green (30 assertions / 8 cases, incl. a DRM-typed power-off test);
slice `[application],[display],[display_settings],[screensaver],[1049]` green (662/161);
`bats tests/shell/test_code_lint.bats` all 6 ok (test seams via `DisplayManagerTestAccess`, no
`_for_testing` in headers). **Still needs CB1 .112 hardware verification** of the dpms sysfs toggle.

## Process
- `git rev-parse --abbrev-ref HEAD` == `fix/1049-screensaver-idle-power-off` before EVERY commit.
- Threading per CLAUDE.md (AsyncLifetimeGuard, no sync widget deletes in queued cbs).
- Build: `make -j`; tests `make test-run`. Hardware-verify on a Pi w/ HDMI panel.

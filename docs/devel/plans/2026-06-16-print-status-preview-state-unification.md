# Print Status Preview State Unification + Stuck "Starting Print…" Fix

Date: 2026-06-16
Branch: `feature/preview-state-unification`
Status: in progress

## Problem (two distinct bugs, observed together during AFC error recovery on the Voron)

1. **Blank 2D/3D gcode preview / thumbnail on panel re-entry.** Navigating Print Status →
   AMS → other config panels → back to Print Status while a print runs leaves the preview
   area grey/blank. Patched ~11 times (see "Prior art") and still recurs.
2. **"Starting Print…" shown at 65% progress.** The print-start phase overlay/message is
   stuck non-IDLE long after the pre-print phase ended.

## Root causes (verified against source)

### Bug #1 — preview re-entry

Responsibility for loading the preview is split across **4 systems**
(`ActivePrintMediaManager`, `PrintStatusPanel`, `NavigationManager`, `ui_gcode_viewer`)
held together by **drift-prone dedup guards** in `PrintStatusPanel`:
`loaded_thumbnail_filename_`, `requested_gcode_filename_`, `pending_gcode_filename_`,
`gcode_loaded_`, `cached_thumbnail_path_`, `thumbnail_load_generation_`.

These guards track **intent** ("we requested file X") not **reality** ("widget X currently
shows content"). They are cleared in inconsistent sets across `on_deactivate()` (only when
print is terminal — `ui_panel_print_status.cpp:1007-1021`), `on_ui_destroyed()`
(`:1092-1094`), and `on_print_start_phase_changed()` (`:2609-2610`).

Failure path: deactivate **mid-print** deliberately does NOT clear the guards. On re-entry
`set_filename()`'s idempotency check (`effective_filename != loaded_thumbnail_filename_`,
`:3224`) early-returns → no reload — but the rendered content (or the widget itself, after a
destroy-on-close / memory-reclaim cycle) is gone. Guard says "showing X"; widget shows
nothing. Nothing reconciles the two on re-entry.

### Bug #2 — stuck "Starting Print…"

`should_start_print_collector()` (`moonraker_manager.h:155-186`) restarts the collector on
**any** non-initial transition to PRINTING from a non-printing/non-paused state. The
mid-print suppression (progress>0 / print_duration>0) is gated on `is_initial_transition`
only (`:182`). During AFC error recovery the print can dip `PRINTING → (ERROR/STANDBY) →
PRINTING`; that final edge restarts the collector at 65% progress, re-entering
`INITIALIZING`/`COMPLETE` ("Preparing Print…" / "Starting Print…"). A plain `PAUSED →
PRINTING` resume is correctly ignored (prev==PAUSED short-circuits at `:165`); the bug needs
the state to leave the PRINTING/PAUSED pair.

## Design

### Part A — unify preview loading (the authorized cleanup)

Single idempotent entry point, called **unconditionally** on every `on_activate()`:

```cpp
// Reconciles displayed preview against current print state. Safe to call any time.
void ensure_preview_current();
```

- Desired state = `effective_filename()` + current view mode + `lifecycle_.want_viewer()`.
- Actual state read from the **widgets**, not bools:
  - thumbnail blank  ⇔ `print_thumbnail_ && lv_image_get_src(print_thumbnail_) == nullptr`
  - gcode unloaded   ⇔ `gcode_viewer_ && !ui_gcode_viewer_has_content(gcode_viewer_)`
- Decision extracted into a **pure, unit-testable** function (mirrors existing
  `decide_gcode_load_action()` in `test_print_status_deferred_loading.cpp`):

```cpp
struct PreviewAction { bool load_thumbnail; bool load_gcode; };
PreviewAction decide_preview_action(const std::string& displayed_file,
                                    const std::string& desired_file,
                                    bool thumbnail_has_src,
                                    bool gcode_has_content,
                                    bool want_viewer,
                                    int  view_mode);
```

Replace the scattered guards with a **single source of truth**: `displayed_file_` (the file
whose content is currently in the widgets; cleared whenever widgets are destroyed/reclaimed,
in lockstep with the pointer nulling in `on_ui_destroyed()`). Keep:
- `cached_thumbnail_path_` — actual content to re-apply (not a guard).
- `thumbnail_load_generation_` — legitimate async-staleness token.
- `pending_gcode_filename_` — payload for the deferred-load timer (not a dedup guard).

Remove `loaded_thumbnail_filename_`, `requested_gcode_filename_`, `gcode_loaded_` as
drift-prone intent guards; their roles fold into `displayed_file_` + widget queries. Delete
the conditional reload blocks in `on_activate()` (`:903-929`) and the special-case clears in
`on_deactivate()`/`on_print_start_phase_changed()` — `ensure_preview_current()` makes them
unnecessary. Add `ui_gcode_viewer_has_content()` query if not already present.

Net effect: re-entry is **self-healing**. A blank widget always reloads because the decision
reads the widget, not a bool that can lie.

### Part B — fix stuck "Starting Print…"  (NOT a blind `should_start` change)

**Landmine:** the existing contract (`test_moonraker_manager.cpp:223-234`, 290-295)
*requires* the collector to restart on a non-initial `STANDBY→PRINTING` with stale-high
progress/print_duration — that is the legitimate "reprint after cancel" path. At the
synchronous state-observer tick, a spurious AFC resume (`PRINTING→ERROR/STANDBY→PRINTING`,
real progress 65%) and a legit reprint are **indistinguishable** (both: prev=STANDBY,
new=PRINTING, stale-high values). Changing `should_start_print_collector()` to suppress on
high progress/duration would break reprints. The only reliable disambiguator is **temporal**
(a genuine new print resets progress→0 / print_duration→0 shortly after; an AFC resume keeps
them high).

**Confirmed mechanism:** a mid-print restart leaves the collector at `INITIALIZING` and it
gets *stuck* because the completion edges don't re-fire: the duration observer
(`moonraker_manager.cpp:674`) completes only when `print_duration` *changes* to >0, but
`print_duration` is **frozen while paused** (AFC error pause). So the overlay persists across
the pause and into the early resume window.

**Approach (no contract break):**
1. **Robust completion (primary):** make completion fire from the *fresh post-restart data*,
   not just the rising edge. On `print_duration`/`print_progress` updates while the collector
   is active, if the value is already substantial (well past a fresh start — e.g.
   `print_duration` over a few seconds, or `has_real_layer_data_`), `complete_from_external_signal`.
   Genuine new prints report *fresh low* values → still show pre-print. AFC resume reports
   *fresh high* values on the first post-resume tick → dismissed immediately. This fixes the
   stuck overlay without touching the reprint contract.
2. **Defense in depth:** a latch `print_running_confirmed_` in `PrinterPrintState`, set once
   real printing is confirmed for the current job (real layer data / sustained progress),
   reset on a confirmed fresh start (progress observed at 0 while active, or filename change).
   While latched, `set_print_start_state()` ignores non-IDLE phases.
3. **Instrumentation:** timestamped debug logs at every collector reset/start/stop and phase
   transition, so the next Voron occurrence yields the exact state sequence to confirm fix #1
   covers it. Until a reproduction confirms the persistence path, treat Part B as
   provisional — do not claim it fixed without a log.

Add unit cases that assert the reprint-after-cancel contract still holds AND that a
post-restart high-`print_duration` tick completes the collector.

## Tests first

- `decide_preview_action()` table: fresh open, re-entry with live widget, re-entry with
  blank thumbnail (destroyed/recreated), re-entry with unloaded viewer, thumbnail-only mode,
  filename change. Each asserts the right reload flags. (FAILS before Part A.)
- `should_start_print_collector()`: AFC `PRINTING→ERROR→PRINTING` and
  `PRINTING→STANDBY→PRINTING` at high `print_duration` must NOT restart; genuine
  STANDBY→PRINTING new print (duration 0) MUST start. (FAILS before Part B.)
- Defense-in-depth: `set_print_start_state(INITIALIZING,…)` while mid-progress is a no-op.

## Prior art (do not re-add as bandaids)

11 prior commits: `e2c36694b`/`bdb67d220` (clear one guard on destroy), `4cf654d00`
(2D fallback on streaming), `8daf12eec` (force paint bed mesh), `0798c94f8` (reload in
on_activate), `d24fabe00` (memory-aware keep-alive), `a5684bfd6` (persistent overlay +
re-apply cached thumbnail), `51f255b44` (clear all three guards + immediate observer),
`5bdd88169` (APMM immediate observer), `8db9804a8` (clear guards on low-mem deactivate),
`f623e7791` (on-demand 2D/3D geometry), `c3f25d1a7` (thumbnail retry/backoff + Moonraker
re-trigger). Most are guard-clearing or path-specific reload patches — exactly the tangle
this unifies. The retry/backoff (`c3f25d1a7`) and memory keep-alive (`d24fabe00`) solve
*different* root causes (late file scan; flicker) and stay.

# AMS Real-Time Filament State — Cross-Backend Refactor

> Execute with superpowers:subagent-driven-development. Branch:
> `feature/ams-realtime-filament-state` (off main). MAJOR work — test-first where feasible,
> review threading-sensitive parts (dynamic per-slot subjects = [L084]/[L077] landmines).

**Goal:** The MULTI-FILAMENT panel must reflect LIVE filament state (path position, active lane,
load/unload availability) by *observing* Moonraker-fed subjects, not reading one-shot snapshots —
for ALL AMS backends, via a common abstraction. DRY out the duplicated/divergent state.

**Problem (observed on U1, live):** push filament to toolhead → "filament inserted" toast fires
(sensor updated) but the path doesn't redraw; after unload the bottom T-badge stays "active" while
the top-right correctly shows Idle; the Load/Unload menu offers Unload for an unloaded slot; the
op-card showed the next slot's color. Root: the panel isn't bound to live per-slot subjects, and
several signals have two divergent sources.

**Verified live sources exist (U1):** `filament_motion_sensor e0..e3_filament` (per-tool toolhead),
`filament_feed left/right` (buffer/port), `filament_detect` (RFID color/type). The backend already
parses these into `sensor_filament_present_[]` / `port_sensor_filament_present_[]` and exposes
`get_slot_filament_segment(i)` (per-slot, live, all backends). The data is there — the panel doesn't
*react* to it.

---

## Design — abstraction at AmsBackend + AmsState (backend-agnostic)

### Backend accessors (per-slot, live) — `include/ams_backend.h` base, with safe defaults

Reuse the existing `get_slot_filament_segment(int)` (already per-slot/live/all-backends). Add what's
missing, NON-pure with graceful defaults so backends that can't provide a signal degrade:

```cpp
// LIVE: filament present at this slot's toolhead/extruder (motion/switch sensor).
// Default false = "no per-slot toolhead sensor" → panel falls back to segment/status.
[[nodiscard]] virtual bool slot_has_filament_at_toolhead(int slot_index) const { return false; }

// Firmware "seated & loaded" for this slot. Default derives from current_slot + filament_loaded.
[[nodiscard]] virtual bool slot_is_actively_loaded(int slot_index) const {
    return slot_index == get_current_slot() && is_filament_loaded();
}
// slot_has_filament_at_spool: reuse existing get_slot_info(i).is_present() (RFID/occupancy).
```
Snapmaker overrides `slot_has_filament_at_toolhead` → `sensor_filament_present_[i]`, and
`slot_is_actively_loaded` → `get_slot_info(i).status == LOADED`. Other backends override only where
they have a real signal; otherwise the defaults apply.

### AmsState per-slot LIVE subjects — `include/ams_state.h` / `src/printer/ams_state.cpp`

Expose per-slot subjects the panel can OBSERVE, updated on every status sync from the backend
accessors. Reuse existing per-slot color/status subjects; ADD:
- `get_slot_segment_subject(int)` — int (PathSegment enum) from `get_slot_filament_segment(i)`
- `get_slot_toolhead_present_subject(int)` — bool from `slot_has_filament_at_toolhead(i)`
- `get_slot_active_loaded_subject(int)` — bool from `slot_is_actively_loaded(i)` (the SINGLE
  source of truth for the active-lane highlight — kills the badge/top-right divergence)

These are DYNAMIC (recreated on backend switch/rediscovery). Provide `(int slot, SubjectLifetime&)`
overloads so observers pass a lifetime token ([L077]). Update them in the existing status-sync path
(where `slot_colors_`/`slot_statuses_` are already updated).

---

## Phase 1 — Abstraction foundation (no UI change yet)
**Files:** `include/ams_backend.h`, `include/ams_state.h`, `src/printer/ams_state.cpp`, each
`src/printer/ams_backend_*.{h,cpp}` (overrides where a real signal exists).
- Add the two virtuals (defaults in base). Override on Snapmaker (motion sensor + LOADED status).
  Override on others ONLY where the signal already exists in their parse (AFC lane motion, HH gate,
  toolchanger per-tool `filament_detected`, IFS/CFS head sensor) — otherwise leave defaults.
- Add the 3 AmsState per-slot subjects + `(slot, SubjectLifetime&)` accessors; update them in the
  status-sync alongside existing per-slot subjects.
- **Tests:** unit-test the backend accessors per backend (mock status → expected per-slot bools);
  unit-test AmsState publishes the subjects on sync (drain queue, read subject) — see [L048].

## Phase 2 — Panel binds reactively (the "listen to real listeners" fix; DRY)
**Files:** `src/ui/ui_panel_ams.cpp`, `src/ui/ui_ams_detail.cpp`, `src/ui/ui_ams_slot.cpp`,
`src/ui/ui_ams_context_menu.cpp`.
- Per-slot path canvas OBSERVES `get_slot_segment_subject(i)` (+ toolhead-present) with paired member
  `SubjectLifetime` tokens ([L084] — member, never local; parallel vectors for the slot list, cleared
  before observers) → redraws on sensor change.
- Active-lane highlight: bind the bottom T-badge AND the top-right "Currently Loaded" to the SAME
  source — `get_slot_active_loaded_subject(i)` / a single current-loaded subject. Remove the second
  divergent read.
- Load/Unload menu enable: use live `slot_has_filament_at_toolhead(i)` / `can_unload_from_toolhead()`
  instead of static RFID `is_present()`.
- Add debug logs on the sensor→subject→observer chain (so live push/pull testing is verifiable).

## Phase 3 — Targeted bug fixes
- **Op-card color off-by-one** (`ams_state.cpp` ~2115, color lookup uses display number as index):
  pin the exact expression, fix to use the 0-based slot_index. Unit-test.
- **`filament_loaded` not cleared on unload** → stale active badge: set it false on
  EVENT_UNLOAD_COMPLETE before UI re-render. Unit-test.
- **Distinct buffer vs toolhead path segment**: add `PathSegment::BUFFER` (or render OUTPUT distinctly)
  so "staged in buffer, retracted from toolhead" is visually different from "at toolhead".
- **Runout dialog suppressed when idle**: the handler in `ui_filament_runout_handler.cpp` is gated on
  Paused — but a dialog STILL appeared after an idle manual unload, so FIND the second trigger path
  (grep other runout/toast surfaces) and gate it on printing-state too.

---

## Validation
- Unit tests per phase (backend accessors, AmsState subjects, off-by-one, unload-clear).
- `make test-run` (sharded; capture exit properly — NOT `| tail`, see L092). Never single-process
  full-suite (OOM/isolation).
- **Live on U1 (Preston):** push/pull filament per lane → path redraws in real time; unload → active
  badge clears + matches top-right; menu Load/Unload matches actual state; colors correct per lane.
  Debug log confirms listener→subject→observer fired.

## Notes
- Don't conflate with the parked preflight branch. Deploy this branch to the U1 for AMS testing.
- Graceful degradation: backends without a toolhead sensor keep current behavior (default false →
  panel uses segment/status), so no regression on AFC/CFS/HH/IFS where the signal is absent.

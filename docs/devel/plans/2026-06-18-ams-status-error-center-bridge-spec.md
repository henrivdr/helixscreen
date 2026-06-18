# Spec: Status→Error-Center Bridge for AMS Backends

**Date:** 2026-06-18
**Status:** Approved design, pre-plan
**Builds on:** `2026-06-16-backend-error-recovery-*` (AFC reference), `2026-06-18-ams-error-recovery-analog-backends-spec.md` (Phase 1 = Happy Hare, shipped)

## 1. Why

`AmsBackend::classify_error(raw_line, ctx)` is invoked by `GcodeErrorRouter` only on gcode
**response lines** (`!!` / `Error:`). Verified in code: only **AFC** and **Happy Hare** emit such
lines — both already done. The other three backends are **status-driven** and never reach that seam:

- **Snapmaker U1** — faults via `channel_error` → `AmsAction::ERROR` (already has the #991 runout dialog).
- **AD5X / IFS** — faults via `execute_gcode` return codes + a timeout that currently snaps to
  `AmsAction::IDLE` (never sets `ERROR`).
- **QIDI Box** — faults via `save_variables slot<N> < 0` → `SlotStatus::BLOCKED`; `recover()` is a stub.

This spec adds a **bridge** so a status-driven `AmsAction::ERROR` flows into the same unified
recovery modal the `!!` path uses — without polluting the gcode router or the data layer.

## 2. Structure (the key decision)

The **presenter** is generic and shared; the **triggers** are source-specific. Two real sources now
want the recovery modal (gcode `!!` and AMS `ERROR`); they MUST share one modal instance, not stack.

```
                         ┌─────────────────────────┐
  gcode !!/Error: ──────▶│ GcodeErrorRouter        │──┐
   (classify_error)      │  (gcode trigger)        │  │
                         └─────────────────────────┘  │   present(ErrorEvent)
                                                       ├──▶ ┌──────────────────────┐
                         ┌─────────────────────────┐  │    │ RecoveryModalPresenter│
  AmsAction::ERROR ─────▶│ AmsErrorBridge          │──┘    │  owns the single      │
   (current_error)       │  (AMS trigger)          │       │  ActionPromptModal,   │
                         └─────────────────────────┘       │  present()/dismiss(), │
                                                           │  dedup                │
                                                           └──────────────────────┘
```

- **`RecoveryModalPresenter`** (NEW, extracted from `GcodeErrorRouter::present_recovery_modal` +
  `recovery_modal_`): owns the single `helix::ui::ActionPromptModal`. API:
  `void present(const helix::ErrorEvent& e);` (shows, or replaces content if already visible — latest
  wins; dedup identical), `void dismiss();`. Source-agnostic. Reuses the free functions
  `decide_presentation()` / `build_recovery_prompt()`.
- **`GcodeErrorRouter`**: unchanged trigger (gcode lines → `classify_error`), but no longer owns the
  modal — delegates to the shared presenter. Stays exclusively about gcode.
- **`AmsErrorBridge`** (NEW): owns the AMS trigger. Observes `AmsState::get_ams_action_subject()`;
  on the **edge into `AmsAction::ERROR`** calls `AmsState::get_backend()->current_error()` and, if it
  returns an event, `presenter.present(*ev)`; on the **edge out of ERROR** calls
  `presenter.dismiss()` (only if it was the presenter of record). AMS-domain dedup (vs bespoke
  dialogs like U1 #991) lives here. Holds an `ObserverGuard` for the subject.
- **`AmsState`** stays pure — it only owns the action subject; it never learns about modals.

**One new backend virtual** — `AmsBackend`:
```cpp
// Current actionable fault for status-driven backends (no `!!` line). Default: none.
// `!!`-driven backends (AFC/HH) leave this default and keep using classify_error().
[[nodiscard]] virtual std::optional<helix::ErrorEvent> current_error() const { return std::nullopt; }
```

**No backend-type checks** in `RecoveryModalPresenter`, `GcodeErrorRouter`, or `AmsErrorBridge`. The
backend owns its error semantics via `current_error()`. (Review gate, as for Phase 1.)

## 3. Ownership & lifetime (`Application`)

`GcodeErrorRouter` is owned by `Application` (`m_gcode_error_router`, built ~`application.cpp:3084`,
torn down ~3963/4276). Add:
- `m_recovery_presenter` (created FIRST — must outlive both consumers),
- `m_gcode_error_router` (takes a `RecoveryModalPresenter&`),
- `m_ams_error_bridge` (takes a `RecoveryModalPresenter&`).

Teardown in reverse: bridge → router → presenter. The presenter owns the modal; both consumers hold a
non-owning reference.

## 4. Threading

`AmsState::get_ams_action_subject()` is set on the main thread (`sync_from_backend`'s subject writes
are main-thread). `AmsErrorBridge` observes via `observe_int_sync` (deferred via UpdateQueue, fires on
main). Presenting/dismissing a modal from a queued main-thread callback is safe (creation, not
deletion). No extra deferral needed. `current_error()` is read on the main thread from the bridge.

## 5. Per-backend `current_error()`

- **AD5X / IFS** (ships blind):
  - **Prerequisite (required for the bridge to ever fire):** change `check_action_timeout()` and the
    `execute_gcode` failure path to set `AmsAction::ERROR` (keeping `operation_detail`) instead of
    snapping to `IDLE`. Today IFS never enters `ERROR`, so `current_error()` would never be queried.
  - `current_error()`: when `system_info_.action == ERROR`, return a CRITICAL `ErrorEvent`
    {source `IFS`, title from the fault, detail = `operation_detail`, recovery actions: `Recover`
    → `IFS_UNLOCK` (the command `recover()` already sends)}.
  - `recover()` / `reset()` / `cancel()` must clear the `ERROR` action state (cancel already resets
    to IDLE; ensure recover/reset do too so the bridge dismisses).
- **QIDI Box** (ships blind):
  - `current_error()`: when any slot is `BLOCKED` (negative `save_variables` slot value), return a
    CRITICAL/WARNING `ErrorEvent` {source `QIDI`, detail naming the blocked lane}. **Recovery is
    blind** — the gcode that clears a BLOCKED slot is unknown and we have no QIDI hardware. Surface
    the fault with **no recovery action** (or a single best-effort `Unload`→`M603` clearly labeled),
    documented as a known gap. Do NOT invent a recovery command.
- **Snapmaker U1**: NOT implemented now (per decision). The bridge supports it (U1 can add
  `current_error()` later returning `nullopt` for #991-owned runout and an event for other faults);
  leaving it default means the bridge never fires for U1, so #991 stays the U1 path. No regression.

## 6. Dedup

Because only IFS/QIDI implement `current_error()` initially, and neither has a competing bespoke
dialog, there is no double-fire today. The bridge still guards: it only presents when
`current_error()` returns a value, and it dismisses on leaving ERROR. If a future backend (U1) both
has a bespoke dialog and implements `current_error()`, it returns `nullopt` for the cases its own
dialog owns — control stays in the backend.

## 7. Testing

- **`RecoveryModalPresenter`**: present(event) shows the modal; present again replaces/keeps single
  instance (no stacking); dismiss() hides. Unit test with a synthesized `ErrorEvent`.
- **`AmsErrorBridge`**: with a mock backend whose `current_error()` returns a value, driving the
  action subject to `ERROR` shows the modal; driving it back to `IDLE` dismisses. With a backend
  returning `nullopt`, no modal. (LVGLTestFixture; drive the subject; `UpdateQueue::drain()`.)
- **IFS**: timeout/command-failure → `AmsAction::ERROR` (not IDLE); `current_error()` returns the
  expected event; `recover()` (IFS_UNLOCK) clears ERROR. Extend `test_ams_backend_ad5x_ifs.cpp`.
- **QIDI**: BLOCKED slot → `current_error()` returns an event with the blocked lane; no invented
  recovery gcode. Extend `test_ams_backend_qidi.cpp`.
- **Abstraction gate (review)**: no new `AmsType::`/`ErrorSource::` switch in `RecoveryModalPresenter`,
  `GcodeErrorRouter`, or `AmsErrorBridge`.

## 8. Phasing (each phase = test-first, own commit[s])

1. **Extract `RecoveryModalPresenter`** from `GcodeErrorRouter`; router delegates; wire ownership in
   `Application`. No behavior change to the gcode path (regression: existing error-routing e2e tests
   stay green). The refactor is the foundation.
2. **`AmsBackend::current_error()` virtual + `AmsErrorBridge`** (observer → presenter), wired in
   `Application`. Tested with a mock backend.
3. **IFS** — timeout/failure → `ERROR`, `current_error()`, recover clears. (blind)
4. **QIDI** — `current_error()` from BLOCKED; no invented recovery. (blind)

## 9. Out of scope

- U1 `current_error()` (deferred; #991 owns U1 recovery).
- Discovering QIDI's BLOCKED-clearing gcode (needs firmware/hardware).
- Any change to `AmsState`'s responsibilities beyond being observed.
- The `classify_error` / `!!` path (complete for AFC + HH).

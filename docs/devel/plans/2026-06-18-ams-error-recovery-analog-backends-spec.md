# Spec: AMS Error-Recovery + Toolchange-Narration for Analog Backends

**Date:** 2026-06-18
**Status:** Approved design, pre-plan
**Builds on:** `2026-06-16-backend-error-recovery-{source,L0,L1,L2}-*.md` (shipped v0.99.80, AFC reference)
**Folds in:** prestonbrown/helixscreen#1052 (buffer if-ladder), prestonbrown/helixscreen#1057 (op-status guard)

## 1. Goal

Port the AFC error-recovery + toolchange-narration system (shipped v0.99.80) to the remaining
four AMS backends — **Happy Hare, Snapmaker U1, AD5X/IFS, QIDI Box** — so each surfaces:

1. **Actionable error recovery** — pausing/critical errors raise a multi-button recovery modal
   with backend-appropriate recovery gcode, instead of raw `!!` text or a swallowed toast.
2. **Toolchange narration** — the sidebar step bar advances through the backend's load/unload
   phases during a toolchange.

## 2. Hard constraint: the abstraction must not leak

All per-backend behavior lives behind existing `AmsBackend` virtuals. The shared layer is **not**
modified to know about any backend:

- `GcodeErrorRouter`, `GcodeNarrationRouter`, `ui_ams_sidebar`, `ActionPromptModal`,
  `ErrorEvent`/`error_classify` stay backend-agnostic.
- New per-backend needs become a **new virtual or capability method** on `AmsBackend` — never a
  `if (type == X)` branch in shared code.
- This is the direction the in-flight coupling merge already enforces (#1051 virtualize remap
  apply, #1054 backend-owned filament sensors, #1055 virtualize DisplayBackend).

**Verification gate:** at review, grep shared UI/router code for new backend-type checks. Any new
`AmsType::`/`ErrorSource::` switch outside a backend `.cpp` is a defect.

## 3. Reference seams (from `include/ams_backend.h`, all already present)

```cpp
// L0 — error classification (default nullopt → generic classifier handles it)
virtual std::optional<helix::ErrorEvent> classify_error(
    const std::string& raw_line, const helix::ClassifyContext& ctx) const;

// L2 — ordered phase template per operation (empty → legacy AmsAction fallback)
struct ToolchangePhase { std::string id; std::string label; bool optional; };
virtual std::vector<ToolchangePhase> toolchange_phase_template(StepOperationType op) const;

// L2 — map a `//` narration body to a phase id (AFC path only)
virtual std::optional<std::string> match_narration_phase(const std::string& narration) const;

// L2 — operation step model + current-index subject (-1 = idle)
virtual OperationStepModel get_operation_step_model(StepOperationType op) const;
virtual lv_subject_t* get_operation_step_index_subject(StepOperationType op);
```

Supporting types (`include/error_event.h`) — **immutable, do not change**:
`ErrorSeverity{INFO,WARNING,CRITICAL}`, `ErrorSource{…,IFS,QIDI,HAPPY_HARE,SNAPMAKER,…}`,
`RecoveryAction{label,gcode,log_tag,style}`, `ErrorEvent{source,severity,title,detail,code,
recovery_actions,sticky}`, `ClassifyContext{is_paused,is_printing}`.

## 4. Narration: two sources, one sink (Phase 0)

AFC narration parses `//` lines (`match_narration_phase` → `GcodeNarrationRouter` →
`AmsState::toolchange_step_` → sidebar). Our four backends emit **no `//` lines**; they synthesize
phase from existing state (HH `action` enum, U1 `ams_operation_phase`, IFS internal phases).

**Decision:** the sidebar reads exactly one index source — `backend->get_operation_step_index_subject(op)`
— and both paths feed it:

- **AFC (parse path):** the narration router continues to drive that subject. Behavior unchanged;
  may require a tiny adjustment so AFC's `toolchange_step_` IS the subject the sidebar reads
  (confirm current wiring before editing — the AFC path must not regress).
- **Synthesis backends:** the backend drives its own `get_operation_step_index_subject(op)` from
  its own state observers. **Zero involvement from `GcodeNarrationRouter`** — synthesis is fully
  encapsulated in the backend.

This keeps the sidebar's source of truth singular and synthesis self-contained per backend.

## 5. Per-backend specifications

### 5.1 Happy Hare (flagship; **ships blind** — no HH hardware)

- **classify_error:** match `reason_for_pause` / `!!` text:
  - `"Runout detected"` → runout
  - `"clog"` / `"possible clog"` → clog
  - `"encoder"` / free-spinning malfunction → jam
  - `"manual intervention"` → critical catch-all
  - gate on `ctx.is_paused`; return nullopt for unrecognized → generic classifier.
- **recovery_actions** (AFC-shaped, HH commands):
  - `RESUME` (primary)
  - `MMU_RECOVER LOADED=1|UNLOADED=1` (derive the arg from `filament_pos`)
  - `MMU_UNLOCK` when locked
  - `MMU_EJECT` / `MMU_UNLOAD` (neutral)
  - reset variant (danger)
- **narration:** map `action` enum (Loading / Forming Tip / Cutting / Heating / Homing /
  Selecting / Purging / Parking) → template phases per op type; drive index subject from the
  `action` observer.
- **Source surface confirmed:** `get_status()` exposes `action`, `filament_pos`,
  `reason_for_pause`, `gate_status`, EndlessSpool config. Wire format reference: HH issue #729
  (`"Runout detected on None  EndlessSpool mode is off - manual intervention is required"`,
  recovery `MMU_RECOVER LOADED=1` + `RESUME`). No upstream macro changes (client-side synthesis).

### 5.2 Snapmaker U1 (**testable** — 192.168.30.103)

- Mostly **wiring existing state** into the new hooks:
  - feed `ams_operation_phase` (Home→Select→Heat→Move) to template + index subject;
  - classify firmware pauses reusing `is_stuck_motion_sensor_runout()` — suppress stale-encoder,
    surface real runout;
  - recovery through existing `prepare_for_resume()` / firmware `AUTO_FEEDING`.

### 5.3 AD5X / IFS (**ships blind** — no AD5X device)

- **classify_error:** parse `!!` AFC-style; recovery `IFS_UNLOCK` + eject.
- **narration:** expose the already-synthesized HEATING→CUTTING→LOADING phases (currently internal
  `operation_detail`) via template + index subject.

### 5.4 QIDI Box (**ships blind**; lightest)

- **classify_error:** classify pause / `!!`; recovery `RESUME` + per-lane eject (the `FORCE_MOVE`
  path from #1041).
- **narration:** thin synthesis — HelixScreen issues the load/unload gcode itself, so set the index
  as each step is driven.

## 6. Folded-in tech debt

- **#1052 — buffer if-ladder:** replace `HAPPY_HARE` checks at `buffer_status_modal.cpp:86,227`
  with a `BufferUiMode` enum on `AmsSystemInfo`, read generically by the modal. Re-verify the
  ladder still exists (post in-flight merge) before editing. **Not a new interface hierarchy** —
  an enum on the existing info struct (per the issue's prescription).
- **#1057 — op-status guard:** port the optimistic op-status guard (backend idle/silence must not
  clobber an in-flight UI load/unload) to AFC/CFS/HH. **Repro first** — confirm the clobber is
  observable in the optimistic window on the current phase-driven architecture before building;
  gate to non-Snapmaker. Reference test salvaged at `op-status-ref-test.cpp` (per #1057).

## 7. Testing & verification

- **Unit (per backend):** `classify_error` fed known wire-format lines → assert
  `ErrorEvent.severity` + `recovery_actions`; narration fed state transitions → assert step index.
  Built on the `MoonrakerClientMock`-driven real-backend approach (#958 direction), not
  `AmsBackendMock`.
- **E2E:** extend `tests/unit/test_toolchange_narration_e2e.cpp` and the recovery-dialog tests
  (`test_unified_recovery_dialog.cpp`, `test_gcode_error_routing_e2e.cpp`) with a case per backend.
- **Fixtures:** capture real firmware output where available (HH from #729-style logs; U1 from
  device) so blind backends are tested against true wire formats, not invented ones.
- **Live verify:** Snapmaker U1 only. HH / QIDI / IFS are **ships-blind** — label as such in commit
  messages and lean on fixtures.
- **Abstraction gate (review):** no new backend-type switch in shared UI/router code.

## 8. Phasing (each phase = test-first, own commit[s])

0. **Narration-source unification** + **#1057 repro**. Confirm sidebar reads one index subject;
   adjust AFC path if needed (no regression). Decide whether #1057 clobber is real.
1. **Happy Hare** full slice — classify_error + recovery + narration synthesis. Establishes the
   synthesis pattern reused by the others.
2. **Snapmaker U1** — wire existing state; **verify live**.
3. **AD5X / IFS** — expose existing synthesized phases + `!!` classification (blind).
4. **QIDI Box** — lightest synthesis + classification (blind).
5. **Fold-in** — #1052 `BufferUiMode` enum + #1057 guard port.

## 9. Out of scope

- Upstream Happy-Hare macro changes / `//` narration (explicitly rejected — client-side synthesis).
- HH PFS sync-feedback data (upstream #958; not in status output, not consumable today).
- The broader per-capability backend abstraction (#992, post-1.0).
- New error-center plumbing — the L0/L1/L2 shared layer is complete and frozen for this work.

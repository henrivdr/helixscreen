# Backend Error-Recovery Overhaul — L2: Toolchange Step/Phase Narration (Spec)

**Status:** Approved for planning · **Date:** 2026-06-16 · **Branch:** `feature/error-recovery-l2`
**Read first:** `2026-06-16-backend-error-recovery-source.md` (master spec, §4.3), `-L2-handoff.md`, `-L1-spec.md` (adapter pattern to mirror), memory `project_afc_error_recovery_overhaul.md`.
**Closes callouts:** **S1** (step shows "Feed filament" while actually purging) · **S2** (missing brush/clean/cut/poop/kick steps).

---

## 1. Problem (grounded, 2026-06-15 Voron)

The toolchange step list shown in the **AMS panel's right-hand sidebar** (`ui_xml/components/ams_sidebar.xml` → `progress_stepper_container`, the vertical `ui_step_progress` bar) is **hardcoded** in `src/ui/ui_ams_sidebar.cpp:446` (`recreate_step_progress_for_operation`) as a `switch` over `StepOperationType` (LOAD_FRESH / LOAD_SWAP / UNLOAD), and advanced by the coarse `AmsAction` enum (`get_step_index_for_action`, line 533). Result during an AFC toolchange:

- **S1** — purge is labeled "Feed filament" (the enum collapses LOADING/PURGING imprecisely against a guessed label set).
- **S2** — brush/clean/cut/poop/kick phases never appear at all; the bar only knows Heat / Feed / Purge / (tip) / Retract.

AFC actually narrates the real sequence on the gcode RESPOND stream as `//` lines (e.g. `// AFC_Brush: Clean Nozzle`, `// Move to Brush`, `// lane 2 is now loaded in toolhead`, plus cut/poop/kick/clean/purge). **There is no `//` consumer today** — L0/L1 only route `!!` errors via `GcodeErrorRouter`. `//` lines are silently dropped.

### Where it shows up (UI location)

The sidebar is the shared right column of both `ams_panel.xml` and `ams_overview_panel.xml`. The `progress_stepper_container` (fixed 150px today, `hidden="true"` at rest) reveals during any active operation (`ams_action != IDLE`); the action buttons hide at the same time, freeing vertical room. During a 2-color print's T-swap, a user on the AMS screen watches this bar advance. **L2 changes only what fills that box and how it advances** — not the operation lifecycle (show/hide, op-type detection) which stays on the existing `AmsAction` path and already handles externally (print-)driven swaps.

---

## 2. Goals / Non-Goals

**Goals**
1. A `//` narration consumer that tags narration lines and routes them to a step-model — **never** surfaced as errors.
2. A generic, per-backend **step-model** on `AmsBackend` (AFC the first map; all others a no-op default → **zero regression**).
3. Replace the hardcoded sidebar step list with backend-derived steps, driven by real narration → fixes S1 + S2.

**Non-Goals**
- No firmware changes. No rework of the operation lifecycle / show-hide logic (still `AmsAction`-driven). No adapters beyond AFC + the no-op default in v1 (YAGNI; the interface makes the rest cheap). No error-surfacing changes (that was L0/L1).

---

## 3. Design decisions (locked with Preston, 2026-06-16)

| Fork | Decision |
|------|----------|
| **(a) Where the `//` consumer lives** | **New sibling ingestor** — a dedicated `GcodeNarrationRouter` with its **own** `notify_gcode_response` subscription (distinct registration key), inspecting only `//` lines. Clean separation from `GcodeErrorRouter` (`!!`) and `ActionPromptManager` (`action:prompt`). Three subscribers to the method is acceptable (each registers under a unique key). |
| **(b) Step-model shape** | **Declared ordered template + narration matcher.** The backend declares an ordered phase template per operation (its typical full sequence, with `optional` flags where phases vary per swap); narration advances the "current" index. Unreached optional phases stay greyed (pending) for the operation's duration. |
| **Display** | Keep the existing **vertical** `ui_step_progress` bar showing the **full** list. Expand it into the freed button space (`height=content`, capped). "We have room" on normal displays; `scroll-to-current` is a small-screen (480×320) safety net, not the common path. |
| **Detail line** | The narrator also publishes the matched phase's human label to the existing `status_label` (`ams_action_detail`) so the status line reads the live phase (e.g. "Brushing nozzle…") in step with the highlighted bar. Thin addition; drop if it complicates. |

---

## 4. Architecture

```
   notify_gcode_response  (WS bg thread)
        │   ["// AFC_Brush: Clean Nozzle", ...]
        ▼
   GcodeNarrationRouter  (NEW sibling; own subscription key)
        │  per `//` line, on bg thread (pure, no LVGL):
        │     backend = AmsState::get_backend()
        │     id = backend->match_narration_phase(line)   // optional<string>
        │  on match → tok.defer(main):
        ▼     set toolchange_step subject = index_of(id in current template)
   AmsState  toolchange_step (int subject, static singleton)
        │     + status_label detail (phase label)
        ▼  observed declaratively
   AmsOperationSidebar
        │  at start_operation(): if backend->toolchange_phase_template(op) non-empty,
        │     build the ui_step_progress label array FROM IT (else legacy switch)
        ▼  observer on toolchange_step → ui_step_progress_set_current(idx) + scroll-to-current
   ui_step_progress vertical bar  (full list, greyed optionals)
```

### 4.1 `GcodeNarrationRouter` (new)

- New `include/gcode_narration_router.h` + `src/application/gcode_narration_router.cpp`, constructed and owned alongside `GcodeErrorRouter` in `application.cpp` (mirror its construction site near line 3008/3044). Registers `notify_gcode_response` under a **distinct** key (e.g. `"gcode_narration_router"`).
- Holds an `AsyncLifetimeGuard`. The bg callback uses `lifetime_.bg_cb(...)` / captures `tok` and `tok.defer(...)`s the subject write (per CLAUDE.md §Threading, [L072]). The line→phase **match is pure** (substring matching, no LVGL) and may run on the bg thread; only the `AmsState` subject update defers to main.
- For each line: trim, require a `//` prefix (after optional whitespace), strip it, hand the remainder to `backend->match_narration_phase()`. No match ⇒ ignore. No backend ⇒ ignore.
- Does **not** classify or surface errors. Does **not** touch `GcodeErrorRouter`.

### 4.2 `AmsBackend` step-model hooks (new, base + AFC override)

Add to `include/ams_backend.h` (base), mirroring the L1 `classify_error` default-nullopt pattern:

```cpp
struct ToolchangePhase {
    std::string id;      // stable key, e.g. "brush"
    std::string label;   // display, translatable, e.g. "Brush nozzle"
    bool        optional;// greyed if never narrated this swap
};

// Declared ordered template for an operation. Empty ⇒ no narration model;
// the sidebar uses the legacy AmsAction-driven hardcoded list (no regression).
[[nodiscard]] virtual std::vector<ToolchangePhase>
toolchange_phase_template(StepOperationType /*op*/) const { return {}; }

// Map a single `//` narration body (prefix stripped) to a phase id.
// nullopt ⇒ not a recognized phase line.
[[nodiscard]] virtual std::optional<std::string>
match_narration_phase(const std::string& /*line*/) const { return std::nullopt; }
```

**`AmsBackendAfc` overrides both** (`include/ams_backend_afc.h` + `src/printer/ams_backend_afc.cpp`, next to its `classify_error`):

- `toolchange_phase_template(op)` returns ordered templates. Proposed AFC maps (phase `id` / label / `optional`):
  - **LOAD_SWAP:** `heat`/Heat nozzle/no, `cut`/Cut tip/yes, `poop`/Purge to bucket/yes, `kick`/Kick away/yes, `feed`/Feed filament/no, `purge`/Purge/yes, `brush`/Brush nozzle/yes, `clean`/Clean nozzle/yes, `load`/Load complete/no.
  - **LOAD_FRESH:** `heat`/no, `feed`/no, `purge`/yes, `load`/no.
  - **UNLOAD:** `heat`/no, `cut`/Cut tip/yes, `retract`/Retract/no.
  - (Final ordering/labels are an implementation detail to refine against live narration; templates are data, easy to tune.)
- `match_narration_phase(line)` does case-insensitive substring matching → phase id, e.g.: contains `AFC_Brush` or `move to brush` → `brush`; `clean nozzle` → `clean`; `cut` → `cut`; `poop` → `poop`; `kick` → `kick`; `purge` → `purge`; `is now loaded in toolhead` → `load`; heating text → `heat`; feed/load-to-hub text → `feed`. Same matching idiom as `classify_error`.

**Phase id is the join key:** narration → id (matcher); template lists ids in order; current index = position of the matched id in the active op's template. An id not present in the template is ignored (logged at debug). Optional template phases never narrated stay `Pending` (greyed).

### 4.3 `AmsState` subject (new)

- New static singleton int subject **`toolchange_step`** (current phase index; `-1` = none/idle). Declared in `include/ams_state.h`, initialized + XML-registered in `AmsState::init_subjects()`, cleaned via `StaticSubjectRegistry::register_deinit("AmsState", …)` (co-located, the existing AmsState registration). Static singleton ⇒ **no** per-item `SubjectLifetime` token needed.
- The narrator sets it (deferred to main). Reset to `-1` when the operation ends (hook into the existing IDLE transition in the sidebar / `AmsState::set_action(IDLE)` path).
- Detail line: the narrator also `lv_subject_copy_string`s the matched phase label into the existing `ams_action_detail` string subject (guarded by the same string-equality check `AmsState` already uses) so `status_label` reflects the live phase.

### 4.4 `AmsOperationSidebar` wiring

- `recreate_step_progress_for_operation(op_type)` (`ui_ams_sidebar.cpp:446`): query `backend->toolchange_phase_template(op_type)`. If **non-empty**, build the `ui_step_t[]` label array from the template (label per phase, all `Pending`); set `current_step_count_` accordingly. If **empty**, fall through to the **existing** hardcoded `switch` verbatim (legacy backends — Snapmaker tip-method logic etc. untouched).
- Record whether the current operation is **narration-driven** (`backend template non-empty`). If so, the step index comes from the new `toolchange_step` observer; the legacy `get_step_index_for_action()` path is **bypassed** for index advancement (it still governs nothing else). If not, the existing `AmsAction`-driven `update_step_progress()` path is unchanged.
- Add a member observer on `toolchange_step` → on change, `ui_step_progress_set_current(step_progress_, idx)` and scroll the container to keep the active row visible. Static subject ⇒ member `ObserverGuard` only, no paired `SubjectLifetime`. Reset on panel teardown via `reset()` ([L085]).
- Container: change `progress_stepper_container` from fixed `height="150"` to `height="content"` with a max (so it grows into freed space but caps on small screens) + enable scroll. Verify on 480×320 and 480×800.

---

## 5. Threading & lifecycle (mandatory — CLAUDE.md §Threading)

- `GcodeNarrationRouter` callback runs on the WS bg thread. It **never** calls `lv_subject_set_*`, touches widgets, or mutates sidebar state directly. Subject writes go through `tok.defer(...)` (token holds its own shared_ptr; **not** `lifetime_.defer` from bg, [#707]/[L072]). The `match_narration_phase` call is pure and bg-safe.
- No synchronous widget deletion in queued/deferred callbacks; the existing `safe_delete_obj` on the step widget already follows this.
- `toolchange_step` is static → singleton lifetime, no token. The sidebar's observer uses `ObserverGuard::reset()` for cleanup ([L085]).
- New XML widget? No — `ui_step_progress` already exists and is registered; no `make regen-xml-schema` needed. New subject names referenced from XML bindings are fine (no schema regen).

---

## 6. Testing strategy (real tests — fail if feature removed)

1. **AFC narration → phase-id mapping** (`[unit]`): table of representative `//` lines → expected phase id, including the **purge line maps to `purge` not `feed` (S1)** and **brush/clean lines map to `brush`/`clean` (S2)**. Non-narration / unknown lines → nullopt.
2. **Template → index** (`[unit]`): given the LOAD_SWAP template, a matched id resolves to the correct ordered index; an id absent from the template resolves to "no change"; optional unreached phases remain `Pending`.
3. **Legacy backends unchanged** (`[unit]`): a backend with the default empty template still produces today's hardcoded label set via the `switch` (regression guard for Snapmaker tip-method paths etc.).
4. **Sidebar glue** (`[ui_integration]`, NOT `[.ui_integration]`): feed synthetic `//` lines through `GcodeNarrationRouter` → assert `toolchange_step` advances and `ui_step_progress` highlights the right row; assert the bar's labels came from the template (brush/clean present, purge labeled "Purge").
5. **Narration router prefix discrimination** (`[unit]`): `!!`/`Error:`/`ok` lines are ignored by the narration router; only `//` reaches the matcher.

Test-only access via `*TestAccess` friend classes, never `_for_testing` methods ([L088]/[L065]).

---

## 7. File touch-list

| File | Change |
|------|--------|
| `include/gcode_narration_router.h` | **new** — router class, `AsyncLifetimeGuard`, ctor takes API/client |
| `src/application/gcode_narration_router.cpp` | **new** — own `notify_gcode_response` sub, `//` filter, pure match, deferred subject write |
| `src/application/application.cpp` | construct/own `GcodeNarrationRouter` alongside `GcodeErrorRouter` (~3008/3044) |
| `include/ams_backend.h` | add `ToolchangePhase`, `toolchange_phase_template()`, `match_narration_phase()` (default impls) |
| `include/ams_backend_afc.h` / `src/printer/ams_backend_afc.cpp` | override both hooks (templates + matcher) |
| `include/ams_state.h` / `src/printer/ams_state.cpp` | new `toolchange_step` subject (init/register/deinit), reset on IDLE; narrator detail-line write helper |
| `src/ui/ui_ams_sidebar.cpp` / `include/ui_ams_sidebar.h` | template-driven label build; `toolchange_step` observer; bypass legacy index path when narration-driven |
| `ui_xml/components/ams_sidebar.xml` | `progress_stepper_container` height=content+cap+scroll |
| `tests/unit/…`, `tests/ui_integration/…` | the 5 test groups above |

---

## 8. Build order (for fan-out)

Roughly three independent fronts that converge at the sidebar:

- **A (model/data):** `AmsBackend` hooks + AFC template/matcher + unit tests (1, 2, 3). No UI.
- **B (ingestion):** `GcodeNarrationRouter` + `application.cpp` wiring + router unit test (5). Threading-critical.
- **C (state/UI):** `AmsState` `toolchange_step` subject + sidebar template build + observer + XML container + ui_integration test (4).

A and B are independent of each other; C depends on the subject existing (define the `toolchange_step` subject early as a shared seam so C can build against it while A/B proceed). Final integration test (4) lands after all three.

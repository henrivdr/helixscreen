# L1 — AFC Error Classification + Multi-Button Recovery Presenter — Design Spec

**Status:** Approved (brainstorm 2026-06-16) · **Author:** brainstormed w/ Preston
**Builds on:** L0 (`2026-06-16-backend-error-recovery-source.md` §4.2, `-L0-plan.md`) — shipped + device-verified.
**Scope:** The AFC adapter (first hand-tuned backend) + a generic multi-button recovery-modal presenter that renders arbitrary `ErrorEvent.recovery_actions[]`. Closes callouts **R1**, **R3**, and audits **E3**.

---

## 1. Context

L0 evolved `GcodeErrorRouter` into a severity-classified surfacing core: `ErrorEvent`/`RecoveryAction` model, pure `error_classify::classify()`, `PrinterState::is_paused()`, the `AmsBackend::classify_error()` hook (default `nullopt`), and `decide_presentation()` routing CRITICAL→modal / WARNING→toast. An uncoded pausing `!!` now raises a full-text CRITICAL modal (verified on the Voron).

What L0 deliberately left for L1:
- **The `MODAL_WITH_RECOVER` path is single-button only.** `present_recovery_modal()` ignores `ev.recovery_actions[]` and does a static `find_recovery(e.code)` lookup (only `key840` registered), rendering one button via `modal_show_confirmation`. There is no presenter that renders an arbitrary list of recovery buttons.
- **No backend implements `classify_error()`.** AFC's toolhead jam still falls through to the L0 generic classifier — it gets a full-text modal but **no recovery buttons** (dead-end OK).

L1 fills both gaps.

### Grounded facts (verified against the worktree)

- **Presenter** (`src/application/gcode_error_router.cpp`):
  - `decide_presentation()` returns `MODAL_WITH_RECOVER` for CRITICAL + non-empty `recovery_actions`.
  - `present_recovery_modal()` calls `find_recovery(e.code)` (static `CfsRecoveryEntry` table, only `key840`), then `helix::ui::modal_show_confirmation(title, detail, Error, button_label, on_confirm, nullptr, ctx)`. `on_confirm` runs `api_->execute_gcode(gcode, success, error_toast, AMS_OPERATION_TIMEOUT_MS)`. Heap `RecoveryCtx` freed on `LV_EVENT_DELETE`.
  - `present_recover_toast()` (key298) and `present_deferred_toast()` are unchanged by L1.
- **ActionPromptModal** (`include/action_prompt_modal.h`, `src/ui/action_prompt_modal.cpp`):
  - Public, reusable programmatically: `bool show_prompt(lv_obj_t* parent, const PromptData& data)`.
  - `PromptData{ std::string title; std::vector<std::string> text_lines; std::vector<PromptButton> buttons; }`.
  - `PromptButton{ std::string label; std::string gcode; std::string color; std::string hex_color; bool is_footer; int group_id; }`. `color` accepts `"primary"|"secondary"|"info"|"warning"|"error"` (mapped to theme colors); empty → default.
  - On tap → `handle_button_click(gcode)` → `gcode_callback_(gcode)` then `hide()`. The modal does NOT send gcode itself — the owner's callback does. Button data + lifetime token are owned by the modal (UAF-safe).
  - It is a `Modal` subclass (ModalStack-managed, RAII). Application owns one persistent instance for the `action:prompt_*` protocol.
- **AmsBackendAfc** (`src/printer/ams_backend_afc.cpp`, `include/ams_backend_afc.h`):
  - `classify_error()` is a `const` member → can read private state directly.
  - Live state available: `tool_start_sensor_` / `tool_end_sensor_` (toolhead entry/nozzle), `current_lane_name_`, `error_state_`, `last_seen_message_` / `last_message_type_`, per-lane `get_slot_info(i)->sensors`, `system_info_.filament_loaded`, `get_current_slot()`.
  - Real AFC macros already used by the backend: `TOOL_UNLOAD [LANE=<lane>]` (unload from toolhead), `AFC_RESET` (reset/re-prep all lanes), `LANE_UNLOAD LANE=<lane>` (per-lane eject, serialized), `AFC_LANE_RESET`, `RESET_FAILURE`. Helper `execute_gcode_notify(gcode, success_msg, ...)` exists.

---

## 2. Goals / Non-Goals

**Goals**
1. AFC's toolhead jam (and lane/hub errors, and any pausing AFC fault) surfaces a CRITICAL modal with **smart, applicable recovery buttons** — never a dead-end OK (R3).
2. The recovery action that actually applies is offered: **Unload** when the toolhead is loaded (instead of a failing Eject), **Eject** when it is empty (R1).
3. One generic presenter renders **arbitrary** `recovery_actions[]` as buttons, replacing the single-button special case. `key840` folds into it.
4. Audit and (if needed) fix the AMS overlay sensor/path render lagging real sensor state mid-error (E3).

**Non-Goals**
- No `precondition` field on `RecoveryAction` (decided: hide inapplicable actions at classify-time, not disable-with-reason).
- No L2 step/phase narration (S1/S2) or L3 polish (R2/P1/P2).
- No changes to AFC / Klipper firmware.
- key298 toast recovery and the deferred-toast path are untouched.

---

## 3. Callouts closed

| ID | Callout | How |
|----|---------|-----|
| **R1** | "Eject failed: Unload from toolhead first" — no Unload offered | `build_recovery_actions()` offers **Unload** (`TOOL_UNLOAD`) when `tool_start_sensor_ \|\| filament_loaded`; offers **Eject** only when empty |
| **R3** | Generic Home/Recover/Abort; need smart context-aware recovery | AFC `classify_error()` emits a context-aware action set; multi-button presenter renders it |
| **E3** | Audit: does the AMS overlay mirror real sensor state mid-error? | Investigate render path; fix if it lags, else document |

---

## 4. Design

### 4.1 Data model — `include/error_event.h`

One additive field on `RecoveryAction`:

```cpp
struct RecoveryAction {
    std::string label;   ///< Button label, e.g. "Unload"
    std::string gcode;   ///< G-code to run on tap
    std::string log_tag; ///< spdlog tag on tap
    std::string style;   ///< "" (neutral) | "primary" | "danger"  — maps to PromptButton.color
};
```

`style` maps to `PromptButton.color` in the presenter (`"primary"`→primary, `"danger"`→`"error"`, `""`→default). No other model change. `decide_presentation()` is unchanged.

### 4.2 AFC classification — `AmsBackendAfc::classify_error()`

```cpp
std::optional<helix::ErrorEvent> classify_error(
    const std::string& raw_line, const helix::ClassifyContext& ctx) const override;
```

Decision order (first match wins):
1. **Toolhead jam/break** — `raw_line` matches the `handle_toolhead_runout` signature (e.g. contains `"tool_end"` + (`"jam"` or `"break"` or `"runout detected"`)). → title *"Toolhead jam"*, CRITICAL, source AFC, `detail` = full untruncated text, `recovery_actions = build_recovery_actions()`.
2. **Lane / hub error** — recognizable markers (lane name + error, hub state error). → tailored title, same builder.
3. **Catch-all** — `ctx.is_paused && error_state_` and none of the above matched. → CRITICAL, source AFC, generic title (e.g. *"Filament system error"*), `detail` = raw text, std `build_recovery_actions()`.
4. Else → `std::nullopt` (let the L0 generic classifier handle it).

`build_recovery_actions() const` reads live state, pushes only applicable actions:

| Condition | Button (style) | gcode | log_tag |
|---|---|---|---|
| always | **Resume** (primary) | `RESUME` | `afc::resume` |
| `tool_start_sensor_ \|\| system_info_.filament_loaded` | **Unload** (neutral) | `TOOL_UNLOAD` | `afc::tool_unload` |
| toolhead empty (above false) | **Eject** (neutral) | `LANE_UNLOAD LANE=<current_lane_name_>` | `afc::lane_unload` |
| always | **Recover** (danger) | `AFC_RESET` | `afc::reset` |

Open item resolved in the plan: confirm whether AFC prefers a resume-specific macro over plain Klipper `RESUME`. `RESUME` is the safe default (always valid while paused). If AFC docs/source show a preferred resume entry point, use it.

### 4.3 Multi-button presenter — `gcode_error_router.cpp`

Replace the `find_recovery`/`modal_show_confirmation` single-button path. New `MODAL_WITH_RECOVER` handling:

1. **Pure builder (LVGL-free, unit-testable):**
   ```cpp
   helix::PromptData build_recovery_prompt(const helix::ErrorEvent& e);
   ```
   Maps `e.title`/`e.detail` → `PromptData.title`/`text_lines`, and each `RecoveryAction` → a `PromptButton{ label, gcode, color = color_for_style(style) }`.
2. **Render** via a **router-owned `std::unique_ptr<helix::ui::ActionPromptModal>`**, lazily created and reused (mirrors Application's persistent protocol modal). Its `gcode_callback` is set once: it finds the tapped action among the last-shown `recovery_actions` (match by gcode), runs `api_->execute_gcode(gcode, success→log(log_tag), error→failure toast, AMS_OPERATION_TIMEOUT_MS)`.
3. **Retire** the static `find_recovery` table and the `present_recovery_modal` confirmation special-case. `key840` already gets its `recovery_actions` populated by `classify()`, so it now flows through the generic presenter as a 1-button modal — byte-for-byte same gcode (`BOX_ERROR_CLEAR`), just rendered generically.

Threading: `process_line` already runs on the main thread (router ctor uses `lifetime_.bg_cb` which defers). Building the modal and reading `PrinterState` stay on the main thread. No new bg-thread access. Modal teardown follows ActionPromptModal's existing token-guarded lifetime (no sync widget deletes introduced).

### 4.4 E3 audit

Investigate `src/ui/ui_panel_ams.cpp` / `ui_ams_sidebar.cpp` / the filament-path canvas: does the lane sensor / path render read live `AFC_stepper` sensor state during an error, or a lagging cached source? Hypothesis (from L0 grounding): it's live, driven by `handle_status_update → EVENT_STATE_CHANGED` in the same Moonraker status batch that updates sensors. **If it lags, fix it; if confirmed live, document the finding** so the callout is closed, not silently dropped. Distinct task so it cannot expand L1 scope unnoticed.

---

## 5. Testing

Real tests (fail if the feature regresses):

- **AFC `classify_error`** (`tests/unit/test_ams_backend_afc_classify.cpp`, feed AFC status JSON then classify):
  - Jam text + toolhead loaded → CRITICAL, source AFC, actions contain **Unload**, do **not** contain Eject.
  - Jam text + toolhead empty → actions contain **Eject**, do **not** contain Unload.
  - Resume + Recover always present.
  - Catch-all: paused + `error_state_` + unrecognized `!!` → CRITICAL AFC with std actions.
  - Not paused / no error_state / non-AFC line → `nullopt`.
- **Pure `build_recovery_prompt`** (`tests/unit/test_gcode_error_routing.cpp`, extend): N actions → N buttons; `style`→`color` mapping; title/detail propagation.
- **Routing** (existing): CRITICAL + actions → `MODAL_WITH_RECOVER` (already covered by L0; key840 still yields a 1-action set).
- **Device verify (Voron, 192.168.1.112):** reproduce the jam or inject `RESPOND TYPE=error MSG="Toolhead runout detected by tool_end sensor, but upstream sensors still detect filament. Possible filament break or jam at the toolhead. Please clear the jam and reload filament manually, then resume the print."` while paused → confirm a modal with **Resume / Unload / Recover** buttons (Unload present because toolhead loaded), not a dead-end OK. Screenshot for the PR.

---

## 6. Sequencing (subagent-driven, test-first per task)

| # | Task | Files |
|---|------|-------|
| A | Add `style` to `RecoveryAction`; `color_for_style()` helper | `include/error_event.h` |
| B | AFC `classify_error()` + `build_recovery_actions()` + tests | `ams_backend_afc.{h,cpp}`, `test_ams_backend_afc_classify.cpp` |
| C | `build_recovery_prompt()` + router-owned ActionPromptModal + retire `find_recovery`; tests | `gcode_error_router.{h,cpp}`, `test_gcode_error_routing.cpp` |
| D | E3 audit (+ fix if it lags, else document) | `ui_panel_ams.cpp` / `ui_ams_sidebar.cpp` / path canvas |
| E | Voron device verification | — |

**Out of L1 scope (later layers):** L2 narration steps (S1/S2), L3 polish (R2/P1/P2).

---

## 7. Open questions (resolved in the implementation plan)

- Exact Resume macro: plain `RESUME` vs. an AFC-specific resume entry point (default `RESUME`).
- Lane/hub error text signatures to hand-match in step 2 (vs. relying on the catch-all). Enumerate from AFC source / observed Voron logs; the catch-all is the safety net.

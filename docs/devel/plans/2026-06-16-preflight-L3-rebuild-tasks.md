# L3 Rebuild Task Breakdown — Snapmaker U1 Native print_task_config

> **For agentic workers:** Execute with superpowers:subagent-driven-development. Spec is the
> addendum `2026-06-16-preflight-L3-snapmaker-native-addendum.md`. This file is the task list.

**Goal:** Replace the U1 gcode-rewrite remap path with the firmware-native `print_task_config`
command API, and add an always-on `SET_PRINT_USED_EXTRUDERS` pre-print send that fixes the
reporter's spurious-feed runout with zero user interaction.

**Architecture:** A pure command-builder on the Snapmaker backend produces the native gcode
string; `PrintStartController` sends it (chained before PRINT_START) only when the backend's
strategy is `SnapmakerNative`. The validator/gate is already correct (identity mapping +
`filament_exist`) and needs only a regression test.

**Tech Stack:** C++17, Catch2 tests, MoonrakerAPI::execute_gcode, AsyncLifetimeGuard.

---

## Findings that shape this plan (from branch investigation 2026-06-16)

- `compute_defaults()` already yields **identity** for U1: Snapmaker inits `slot.mapped_tool = i`
  (`ams_backend_snapmaker.cpp:79`), and `compute_defaults` Priority-1 (FIRMWARE_MAPPING) matches
  it (`filament_mapper.cpp:165`). Color-match (Priority-2) never runs for U1.
- `collect_available_slots()` for U1 already surfaces `slot_index == physical head (0-3)` and
  `is_empty == !filament_exist[head]` (`ams_state.cpp:690-712`, `ams_backend_snapmaker.cpp:1033-1050`).
- `PreflightValidator::validate()` is backend-agnostic and therefore **already** blocks exactly
  the post-map heads that are empty. **Part B = no code change; add a regression test (Task 4).**
- `AmsBackendSnapmaker::set_tool_mapping()` is `not_supported` and the U1 mapping card is hidden —
  so the native apply path must NOT go through `set_tool_mapping`; it emits native gcode instead.

## Index discipline (do not get this wrong)

- `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<logical 0-31> MAP_EXTRUDER=<physical 0-3>`
- `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv physical 0-3, ascending, deduped>`
- Firmware default `extruder_map_table = [0,1,2,3,0,0,…]` → `default_head(t) = (0<=t<=3) ? t : 0`.
- Commands MUST precede PRINT_START (they error mid-print, id 531). Each print resets the map to
  identity + used to all-False, so we re-send every print.

---

# BATCH 1 — Part A foundation (deploy + bench-validate before Batch 2)

### Task 1: Add `RemapStrategy::SnapmakerNative` (Part D)

**Files:**
- Modify: `include/ams_backend.h` (enum ~line 1092)
- Modify: `include/ams_backend_snapmaker.h` (`get_remap_strategy()` ~lines 143-145)
- Modify: `tests/unit/test_remap_strategy.cpp` (Snapmaker assertion ~lines 134-137)

- Enum becomes `enum class RemapStrategy { None, Native, GcodeRewrite, SnapmakerNative };`.
- Snapmaker override returns `RemapStrategy::SnapmakerNative` (was `GcodeRewrite`).
- Update the test: Snapmaker now expects `SnapmakerNative`. Keep the doc comment on the enum
  describing the new value ("backend emits firmware-native print_task_config gcode pre-print").
- ACE stays `None`; all other Native backends unchanged.
- Verify: `./build/bin/helix-tests "[ams][strategy]"` green.

### Task 2: Pure command builder `build_preprint_gcode` (Part A + C core)

**Files:**
- Modify: `include/ams_backend_snapmaker.h` (declare public const method)
- Modify: `src/printer/ams_backend_snapmaker.cpp` (implement; no `api_` access — pure)
- Test: `tests/unit/test_snapmaker_preprint_gcode.cpp` (new, tag `[snapmaker][preprint]`)

Signature:
```cpp
// Builds the firmware-native pre-print command sequence for print_task_config.
// tools_used: logical tools the gcode body uses (ParsedGCodeFile::tools_used_indices).
// remap: logical tool -> physical head, ONLY entries the user changed from identity.
//        Tools absent from `remap` use default_head(t) = (0<=t<=3) ? t : 0.
// Returns newline-joined gcode (no trailing newline), or "" when tools_used is empty.
[[nodiscard]] std::string build_preprint_gcode(
    const std::set<int>& tools_used,
    const std::map<int, int>& remap) const;
```

Behavior:
1. If `tools_used` empty → return `""`.
2. For each remapped tool `t` (present in `remap`), emit one line, in ascending `t` order:
   `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<t> MAP_EXTRUDER=<remap[t]>`.
3. Compute used heads = `{ remap.count(t) ? remap[t] : default_head(t) : for t in tools_used }`,
   deduped + ascending. Emit `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv>`.
4. Join all lines with `\n`.

Test cases (TDD — write first, watch fail):
- `{0,2}`, `{}` → `"SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2"`
- `{0,1,2,3}`, `{}` → `"SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,1,2,3"`
- `{1}`, `{}` → `"SET_PRINT_USED_EXTRUDERS EXTRUDERS=1"`
- `{1}`, `{{1,3}}` →
  `"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=3\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=3"`
- `{0,2}`, `{{0,1},{2,3}}` →
  `"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\nSET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=1,3"`
- `{}`, `{}` → `""`
- Dedup: `{0,1}`, `{{1,0}}` → used `{0}` → `"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\nSET_PRINT_USED_EXTRUDERS EXTRUDERS=0"`
- Extended-tool default: `{5}`, `{}` → head 0 → `"SET_PRINT_USED_EXTRUDERS EXTRUDERS=0"`
  (document this as firmware-identity behavior; revisit extended tools in a later pass).

Construct the backend for the test the same way `test_remap_strategy.cpp` does (probe / null api).

### Task 3: Wire Part A into print-start (always-on, gated to SnapmakerNative)

**Files:**
- Modify: `src/ui/ui_print_start_controller.cpp` (`execute_print_start` / `apply_filament_remaps`)
- Modify: `include/ui_print_start_controller.h` if a helper is added
- Possibly modify: `include/ui_print_select_detail_view.h` + `.cpp` to expose
  `std::set<int> get_tools_used() const` (reads `ParsedGCodeFile::tools_used_indices`) and
  `std::map<int,int> get_effective_remap() const` (from `filament_mapping_card_.get_mappings()`,
  identity-filtered; empty for U1 today).

Requirements:
- Only when `backend->get_remap_strategy() == RemapStrategy::SnapmakerNative`:
  1. Gather `tools_used` and `remap` (remap empty for U1 now → Part A path; non-empty later → Part C).
  2. `gcode = static_cast<AmsBackendSnapmaker*>(backend)->build_preprint_gcode(tools_used, remap);`
     (cast is safe — strategy uniquely identifies the Snapmaker backend; assert/log otherwise.)
  3. If `gcode` non-empty: `api_->execute_gcode(gcode, on_success, on_error, timeout)` and
     **chain the actual print start into `on_success`** — the native commands MUST land before
     PRINT_START. Do NOT fire-and-forget then start (race: prestart auto-feed runs first).
  4. On error: surface a toast/modal and do NOT start the print (better to fail loud than feed an
     empty head). Confirm the existing error-surfacing helper used elsewhere in this controller.
- Threading: `execute_gcode` callbacks fire on the WebSocket bg thread. Use the controller's
  `AsyncLifetimeGuard` token + `tok.defer(...)` to resume on the main thread before touching
  members / starting the print. **No `if (tok.expired()) return;` then member access** (L081).
- Non-Snapmaker / non-SnapmakerNative path is completely unchanged.
- Confirm the send also happens on the reprint / queued-print path if that path reaches
  `execute_print_start`; if it bypasses, note it (do not over-build — flag for Batch 2).

### Task 4: Reporter regression test (locks Part B + Part A intent)

**Files:**
- Test: extend `tests/unit/test_preflight_validator.cpp` (or new `[preflight][snapmaker]` case)

Two assertions modeling the Discord report (U1, body uses heads 0+2, head 1 unloaded):
1. **Gate does not over-block:** build `tools = [T0, T2]`, `slots = [slot0 loaded, slot1 EMPTY,
   slot2 loaded, slot3 loaded]` (Snapmaker shape: slot_index = head, is_empty from filament_exist),
   identity `mapping`. Assert `validate(...).has_block() == false` (head 1 empty but not required).
2. **Gate blocks a genuinely-empty required head:** same but slot2 EMPTY → `has_block() == true`.

(The Part A "used = 0,2" half is already covered by Task 2's `{0,2}` case; reference it here.)

### Batch 1 close-out
- `make -j` (program) green; `make test-run` full suite green (capture exit separately — L092).
- Cross-build + deploy: `make snapmaker-u1-docker` then
  `SNAPMAKER_U1_HOST=192.168.30.103 make deploy-snapmaker-u1`.
- **STOP for bench validation (Preston runs the real print).** Load-bearing check: with
  `lid_PLA_6m28s.gcode` (body 0+2) and head 1 empty, confirm HelixScreen sends
  `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` (in the device log) and the print completes with head 1
  NOT fed (no runout). Verify the command lands before the baked prestart block.

---

# BATCH 1b — Route reprint through the controller (Preston-directed, root-cause fix)

> Decision (Preston 2026-06-16): reprint should share the controller's pre-print path so the U1
> native send (and any future universal pre-print logic) lives in ONE place, killing the
> `handle_reprint_button` → `api_->job().start_print()` bypass/duplication.

**Facts (investigation 2026-06-16):**
- `PrintSelectPanel` is a static singleton (`g_print_select_panel`) — alive for the whole session,
  outlives the per-overlay `PrintStatusPanel`. Reachable via `get_global_print_select_panel()`.
  `print_controller_` is private — add a getter.
- After a print FINISHES, the status panel's own `gcode_viewer_` has parsed the file
  (`gcode_loaded_==true`), so `ui_gcode_viewer_get_parsed_file()->tools_used_indices` is available
  with no re-fetch. EARLY-cancel (e.g. runout before the deferred load fires) → not parsed → empty.
- The controller currently always starts via `PrintPreparationManager` (upload/prep). Reprint must
  use the lightweight `api_->job().start_print(filename, ok, err)` (`moonraker_job_api.h:69`) because
  the file is already on the printer.

### Task R5: Parameterize `send_snapmaker_preprint_then` (pure refactor, no behavior change)

**Files:** `src/ui/ui_print_start_controller.cpp`, `include/ui_print_start_controller.h`
- Change signature to:
  `void send_snapmaker_preprint_then(const std::set<int>& tools_used, const std::map<int,int>& remap,
   std::function<void()> on_done, std::function<void()> on_abort);`
- Move the error body's "re-enable button + cancel" into the caller-supplied `on_abort`; keep the
  `NOTIFY_ERROR_MODAL` inside (common to both callers). Empty-gcode and no-api paths call `on_done` /
  `on_abort` respectively.
- Update the existing `execute_print_start()` call site to pass
  `detail_view_->get_tools_used()`, `detail_view_->get_effective_remap()`, `start_now`, and
  `[this]{ if(update_print_button_) update_print_button_(); if(on_print_cancelled_) on_print_cancelled_(); }`.
- Build + `[ams][strategy] [snapmaker][preprint] [preflight_validator]` green; behavior identical.

### Task R6: `initiate_reprint` on the controller + PrintSelectPanel getter

**Files:** `src/ui/ui_print_start_controller.{cpp,h}`, `src/ui/ui_panel_print_select.{cpp,h}`
- Add `PrintStartController::initiate_reprint(const std::string& filename, const std::string& path,
   const std::set<int>& tools_used, std::function<void()> on_started, std::function<void()> on_error)`:
  - local `start` lambda → `api_->job().start_print(filename, ok, err)` with `auto tok =
    lifetime_.token();` and `tok.defer(...)` on both callbacks; ok → `on_started`; err → surface
    `NOTIFY_ERROR(lv_tr("Failed to reprint: {}"), ...)` + `on_error`.
  - gate: `if (backend && strategy==SnapmakerNative) send_snapmaker_preprint_then(tools_used, {},
    start, on_error); return; } start();` (remap empty — no reprint remap UI; identity reproduces the
    spurious-feed fix). The native-send error path already shows the modal + calls `on_abort`
    (=`on_error`).
  - `initiate_reprint` invokes `on_started`/`on_error` on the MAIN thread (it defers internally).
- Add `PrintSelectPanel::get_print_start_controller()` returning `print_controller_.get()`.

### Task R7: Route `handle_reprint_button` through the controller

**Files:** `src/ui/ui_panel_print_status.{cpp,h}`
- Add `std::set<int> PrintStatusPanel::get_tools_used() const` reading
  `ui_gcode_viewer_get_parsed_file(gcode_viewer_)->tools_used_indices` (empty if `!gcode_loaded_`/
  null) — mirror the detail view helper.
- In `handle_reprint_button()`: keep the grace-period/empty/api guards and the immediate
  `ui_set_button_enabled(btn_cancel_, false)`. Then:
  ```cpp
  auto* panel = get_print_select_panel(printer_state_, api_); // ensure created
  auto* controller = panel ? panel->get_print_start_controller() : nullptr;
  if (controller) {
      auto tok = lifetime_.token();
      controller->initiate_reprint(filename, current_path_or_empty, get_tools_used(),
          /*on_started=*/[](){ /* PrinterState observer flips button to Cancel */ },
          /*on_error=*/[this, tok]() mutable {
              tok.defer("PrintStatusPanel::reprint_reenable",
                        [this]{ ui_set_button_enabled(btn_cancel_, true); }); });
  } else {
      // fallback: existing direct api_->job().start_print(...) path (unchanged)
  }
  ```
- Guard the status-panel callback with the status panel's OWN `lifetime_` token (the controller
  guards itself, NOT the status panel). The path component for `initiate_reprint`: reprint filenames
  from `print_stats` are Moonraker-relative; pass the path the existing direct call used (likely
  empty/derived) — match whatever `api_->job().start_print(filename)` already does (filename only).

### Batch 1b verify
- `make -j` + full `make test-run` green. `scripts/check_l081_anti_pattern.py` clean.
- Self-review trace: (a) U1 reprint with parsed file → sends `SET_PRINT_USED_EXTRUDERS` then starts
  once; (b) reprint with no parsed file → empty tools_used → no send → starts (safe, == today);
  (c) non-U1 reprint → no send → starts; (d) controller-null → direct fallback unchanged.

### Tracked follow-up (NOT this batch): R8 async re-parse fallback
When `get_tools_used()` is empty at reprint (early-cancel/web-started), download+parse the file to
recover `tools_used` before the send, so the runout-recovery reprint is robust. Blocked on: no
headless fetch+parse utility today (welded to the viewer widget) — needs a small
`download_file_to_path` + `GcodeParser::parse_file` composition. Track separately.

---

# BATCH 2 — Part C native remap UI (after Batch 1 bench-validates)

> Detailed task breakdown deferred until Part A is hardware-proven, because Part C rides the same
> native-command channel. High-level shape (to expand post-validation):

- Route `on_preflight_remap()` `SnapmakerNative → open_native_remap_modal()` (reuse the
  FilamentMappingModal / card modal); make the modal usable for U1 (4 physical heads as targets)
  even though the inline card is hidden.
- User's chosen map flows into `filament_mapping_card_` mappings_ so `recompute_preflight()`
  re-evaluates the gate against it (already generic).
- At print-start, `get_effective_remap()` becomes non-empty → Task 3's path emits
  `SET_PRINT_EXTRUDER_MAP` lines + recomputed `SET_PRINT_USED_EXTRUDERS`. No new send path.
- `[ams][strategy]` already updated in Task 1. Keep `GcodeToolRemapper` /
  `modify_and_print_with_remap` as the generic GcodeRewrite fallback (ACE first future consumer).

---

## Risks (carry into bench validation)
- `SET_PRINT_FLOW_CALIBRATE` baked by slicer may not be a defined macro — confirm no-op.
- Confirm native sends land before PRINT_START on a real print (chaining must hold end-to-end).
- Queued/reprint path coverage (Task 3 note).

# Happy Hare Error-Recovery + Narration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Happy Hare AMS backend actionable error-recovery dialogs and a labeled toolchange step bar, mirroring the AFC reference, entirely behind the existing `AmsBackend` virtuals.

**Architecture:** Override `classify_error()` + `build_recovery_actions()` (mirroring `AmsBackendAfc`) so pausing MMU faults raise a multi-button recovery modal with Happy-Hare gcode. Override `toolchange_phase_template()` and drive `AmsState::set_narration_phase()` from the existing `parse_mmu_state()` `action` ingestion (client-side synthesis — Happy Hare emits no `//` narration). The shared error router, narration router, and sidebar are NOT modified.

**Tech Stack:** C++17, LVGL 9.5 subjects, Catch2, `AsyncLifetimeGuard` for bg→main deferral, nlohmann::json.

## Global Constraints

- **Abstraction must not leak:** all per-backend behavior stays behind `AmsBackend` virtuals. No new `AmsType::`/`ErrorSource::` switch in shared UI/router code. (Review gate: grep shared code for new backend-type checks.)
- **This backend ships blind** — no Happy Hare hardware. Wire formats come from upstream issue #729 and the parsed fields already in `parse_mmu_state()`. Recovery commands are best-effort per HH conventions; label commits `(ships blind)`.
- **Threading:** `parse_mmu_state()` runs on the WebSocket bg thread. Any `lv_subject_*` / `AmsState` mutation MUST be deferred via `auto tok = lifetime_.token(); tok.defer("tag", [...]{...});` — never call `set_narration_phase` directly from bg. (CLAUDE.md § Threading; never `if (tok.expired()) return;` then member access.)
- **spdlog only**, SPDX headers, `lv_tr()` on user-facing labels (not product names / material codes).
- **Build:** `make -j` (program), `make test-run` (build+run tests). Run a single tag: `./build/bin/helix-tests "[tag]"`.
- **No-op confirmed (Phase 0):** `AmsBackend::get_operation_step_index_subject(op)` (`src/printer/ams_backend.cpp:38`) already returns `AmsState::get_toolchange_step_subject()` when `toolchange_phase_template(op)` is non-empty. No unification work needed — declaring a template + writing the subject is sufficient.

---

### Task 1: Happy Hare error classification + recovery actions

Mirrors `AmsBackendAfc::classify_error` (`src/printer/ams_backend_afc.cpp:383`) and `build_recovery_actions` (`:361`). Happy Hare's rich error text lives in the member `reason_for_pause_` (populated in `parse_mmu_state`), not the `!!` line, so classification reads that member like AFC reads `error_state_`.

**Files:**
- Modify: `include/ams_backend_happy_hare.h` (add two method decls + `build_recovery_actions` private helper)
- Modify: `src/printer/ams_backend_happy_hare.cpp` (implementations)
- Test: `tests/unit/test_ams_backend_happy_hare.cpp` (existing file, existing `AmsBackendHappyHareTestHelper`)

**Interfaces:**
- Consumes: `helix::ErrorEvent`, `helix::ErrorSource::HAPPY_HARE`, `helix::ErrorSeverity`, `helix::RecoveryAction`, `helix::ClassifyContext` (`include/error_event.h`); members `reason_for_pause_` (`std::string`), `system_info_.filament_loaded` (`bool`), `system_info_.action` (`AmsAction`), `filament_pos_` (`int`); `mutex_` (inherited).
- Produces: `std::optional<helix::ErrorEvent> AmsBackendHappyHare::classify_error(const std::string&, const helix::ClassifyContext&) const` (override); `std::vector<helix::RecoveryAction> AmsBackendHappyHare::build_recovery_actions() const` (private).

- [ ] **Step 1: Write the failing tests**

Add to `tests/unit/test_ams_backend_happy_hare.cpp`. The helper exposes `test_parse_mmu_state(json)` to set member state, and overrides `execute_gcode` to capture. Add a small accessor for `classify_error` (it's public via the override).

```cpp
TEST_CASE("Happy Hare classify_error: runout pause is CRITICAL with recovery",
          "[ams][happy_hare][error-center]") {
    AmsBackendHappyHareTestHelper hh;
    hh.initialize_test_gates(4);

    // Firmware reports a runout pause via reason_for_pause + action ERROR.
    nlohmann::json mmu;
    mmu["action"] = "Error";
    mmu["filament_pos"] = 8;  // loaded at toolhead
    mmu["reason_for_pause"] =
        "Runout detected on gate 0  EndlessSpool mode is off - manual intervention is required";
    hh.test_parse_mmu_state(mmu);

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto ev = hh.classify_error("!! Runout detected", ctx);

    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::HAPPY_HARE);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK_FALSE(ev->recovery_actions.empty());
    // Resume is always offered, first/primary.
    CHECK(ev->recovery_actions.front().gcode == "RESUME");
    // Detail carries the descriptive reason, not the bare !! line.
    CHECK(ev->detail.find("Runout detected on gate 0") != std::string::npos);
}

TEST_CASE("Happy Hare classify_error: recover gcode reflects loaded state",
          "[ams][happy_hare][error-center]") {
    AmsBackendHappyHareTestHelper hh;
    hh.initialize_test_gates(4);
    nlohmann::json mmu;
    mmu["action"] = "Error";
    mmu["filament_pos"] = 8;
    mmu["reason_for_pause"] = "Clog detected";
    hh.test_parse_mmu_state(mmu);

    helix::ClassifyContext ctx; ctx.is_paused = true;
    auto ev = hh.classify_error("!! Clog detected", ctx);
    REQUIRE(ev.has_value());
    bool has_recover_loaded = false;
    for (const auto& a : ev->recovery_actions)
        if (a.gcode == "MMU_RECOVER LOADED=1") has_recover_loaded = true;
    CHECK(has_recover_loaded);
}

TEST_CASE("Happy Hare classify_error: non-!! line and non-paused defer to generic",
          "[ams][happy_hare][error-center]") {
    AmsBackendHappyHareTestHelper hh;
    hh.initialize_test_gates(4);
    helix::ClassifyContext ctx;  // not paused
    CHECK_FALSE(hh.classify_error("Error: generic klipper error", ctx).has_value());
    CHECK_FALSE(hh.classify_error("ok", ctx).has_value());
    ctx.is_paused = true;  // paused but backend not in error state
    CHECK_FALSE(hh.classify_error("!! something unrelated", ctx).has_value());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[happy_hare][error-center]"`
Expected: FAIL — `classify_error` returns base-default `nullopt` (the override doesn't exist yet), so the first two `REQUIRE(ev.has_value())` fail.

- [ ] **Step 3: Add declarations to the header**

In `include/ams_backend_happy_hare.h`, in the public section near the other overrides:

```cpp
    // Error-center: classify a pausing MMU fault into a recovery ErrorEvent.
    [[nodiscard]] std::optional<helix::ErrorEvent>
    classify_error(const std::string& raw_line,
                   const helix::ClassifyContext& ctx) const override;
```

In the private section:

```cpp
    // Build context-aware recovery actions from live MMU state. Caller holds mutex_.
    [[nodiscard]] std::vector<helix::RecoveryAction> build_recovery_actions() const;
```

Ensure `#include "error_event.h"` is present in the header (add if missing).

- [ ] **Step 4: Implement in the .cpp**

In `src/printer/ams_backend_happy_hare.cpp` (add `#include <cctype>` if not present):

```cpp
std::vector<helix::RecoveryAction> AmsBackendHappyHare::build_recovery_actions() const {
    // Caller holds mutex_.
    std::vector<helix::RecoveryAction> actions;

    // Resume after the user clears the fault (always offered, primary).
    actions.push_back({lv_tr("Resume"), "RESUME", "hh::resume", "primary"});

    // MMU_RECOVER re-syncs HH's filament state; the LOADED/UNLOADED arg must match
    // reality (HH issue #729). Derive from the live loaded flag.
    const bool loaded = system_info_.filament_loaded;
    actions.push_back({lv_tr("Recover"),
                       loaded ? "MMU_RECOVER LOADED=1" : "MMU_RECOVER UNLOADED=1",
                       "hh::recover", ""});

    // If filament is at the toolhead, offer an explicit unload.
    if (loaded) {
        actions.push_back({lv_tr("Unload"), "MMU_UNLOAD", "hh::unload", ""});
    }

    // Force-clear the MMU pause lock (last resort).
    actions.push_back({lv_tr("Unlock"), "MMU_UNLOCK", "hh::unlock", "danger"});
    return actions;
}

std::optional<helix::ErrorEvent> AmsBackendHappyHare::classify_error(
    const std::string& raw_line, const helix::ClassifyContext& ctx) const {
    // Only `!!` emergency lines are candidates (matches AFC).
    if (raw_line.size() < 2 || raw_line[0] != '!' || raw_line[1] != '!') {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Happy Hare reports the descriptive cause in reason_for_pause_; prefer it
    // over the terse !! line for the modal detail.
    std::string bare = (raw_line.size() > 3 && raw_line[2] == ' ') ? raw_line.substr(3)
                                                                   : raw_line.substr(2);
    std::string detail = !reason_for_pause_.empty() ? reason_for_pause_ : bare;

    auto contains_ci = [](const std::string& hay, const char* needle) {
        std::string h = hay, n = needle;
        for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return h.find(n) != std::string::npos;
    };

    // A recognized MMU fault: a descriptive reason is present, OR the print is
    // paused while HH is in its ERROR action. Mirrors AFC's error_state_ gate.
    const bool hh_error_state = (system_info_.action == AmsAction::ERROR);
    const bool recognized =
        contains_ci(detail, "runout") || contains_ci(detail, "clog") ||
        contains_ci(detail, "encoder") || contains_ci(detail, "jam") ||
        contains_ci(detail, "manual intervention");

    if ((ctx.is_paused && hh_error_state) || (recognized && !reason_for_pause_.empty())) {
        helix::ErrorEvent e;
        e.source = helix::ErrorSource::HAPPY_HARE;
        e.severity = helix::ErrorSeverity::CRITICAL;
        e.title = contains_ci(detail, "runout") ? lv_tr("Filament runout")
                                                 : lv_tr("Filament System Error");
        e.detail = detail;
        e.sticky = true;
        e.recovery_actions = build_recovery_actions();
        return e;
    }

    // Not an HH-owned fault — let the generic classifier handle it.
    return std::nullopt;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[happy_hare][error-center]"`
Expected: PASS (3 cases, all sections green).

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_happy_hare.h src/printer/ams_backend_happy_hare.cpp \
        tests/unit/test_ams_backend_happy_hare.cpp
git commit -m "feat(error-recovery): Happy Hare classify_error + recovery actions (ships blind)"
```

---

### Task 2: Happy Hare toolchange phase template

Declares the ordered phases for the step bar. Empty template = legacy generic action-switch in the sidebar; a non-empty template routes the sidebar to the `AmsState` step subject (which Task 3 drives). Phases are synthesized from Happy Hare's `AmsAction` vocabulary (`include/ams_types.h:242`): HEATING, FORMING_TIP, CUTTING, UNLOADING, SELECTING, LOADING, PURGING.

**Files:**
- Modify: `include/ams_backend_happy_hare.h` (decl)
- Modify: `src/printer/ams_backend_happy_hare.cpp` (impl)
- Test: `tests/unit/test_ams_backend_happy_hare.cpp`

**Interfaces:**
- Consumes: `AmsBackend::ToolchangePhase`, `StepOperationType` (`include/ams_step_operation.h`).
- Produces: `std::vector<AmsBackend::ToolchangePhase> AmsBackendHappyHare::toolchange_phase_template(StepOperationType op) const` (override). Phase ids used by Task 3: `"heat"`, `"form_tip"`, `"cut"`, `"unload"`, `"select"`, `"feed"`, `"purge"`, `"load"`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("Happy Hare toolchange_phase_template: ops declare ordered phases",
          "[ams][happy_hare][narration]") {
    AmsBackendHappyHareTestHelper hh;
    auto swap = hh.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(swap.empty());
    CHECK(swap.front().id == "heat");
    CHECK(swap.back().id == "load");
    // Fresh load skips the unload/cut phases.
    auto fresh = hh.toolchange_phase_template(StepOperationType::LOAD_FRESH);
    REQUIRE_FALSE(fresh.empty());
    bool fresh_has_unload = false;
    for (const auto& p : fresh) if (p.id == "unload") fresh_has_unload = true;
    CHECK_FALSE(fresh_has_unload);
    // Unload op ends at "unload".
    auto unload = hh.toolchange_phase_template(StepOperationType::UNLOAD);
    REQUIRE_FALSE(unload.empty());
    CHECK(unload.back().id == "unload");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/bin/helix-tests "[happy_hare][narration]"`
Expected: FAIL — base default returns `{}`, so `REQUIRE_FALSE(swap.empty())` fails.

- [ ] **Step 3: Add declaration**

In `include/ams_backend_happy_hare.h` public section:

```cpp
    [[nodiscard]] std::vector<ToolchangePhase>
    toolchange_phase_template(StepOperationType op) const override;
```

- [ ] **Step 4: Implement**

```cpp
std::vector<AmsBackend::ToolchangePhase>
AmsBackendHappyHare::toolchange_phase_template(StepOperationType op) const {
    switch (op) {
    case StepOperationType::LOAD_SWAP:
        return {
            {"heat",     "Heat nozzle",   false},
            {"form_tip", "Form tip",      true},
            {"cut",      "Cut tip",       true},
            {"unload",   "Unload",        false},
            {"select",   "Select gate",   true},
            {"feed",     "Load filament", false},
            {"purge",    "Purge",         true},
            {"load",     "Load complete", false},
        };
    case StepOperationType::LOAD_FRESH:
        return {
            {"heat",   "Heat nozzle",   false},
            {"select", "Select gate",   true},
            {"feed",   "Load filament", false},
            {"purge",  "Purge",         true},
            {"load",   "Load complete", false},
        };
    case StepOperationType::UNLOAD:
        return {
            {"heat",     "Heat nozzle", false},
            {"form_tip", "Form tip",    true},
            {"cut",      "Cut tip",     true},
            {"unload",   "Unload",      false},
        };
    }
    return {};
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./build/bin/helix-tests "[happy_hare][narration]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_happy_hare.h src/printer/ams_backend_happy_hare.cpp \
        tests/unit/test_ams_backend_happy_hare.cpp
git commit -m "feat(error-recovery): Happy Hare toolchange phase template (ships blind)"
```

---

### Task 3: Happy Hare narration synthesis (action → step index)

Happy Hare emits no `//` narration, so the backend drives the `AmsState` step subject itself from the `action` field already parsed in `parse_mmu_state()`. The sidebar's existing op-detection (`ui_ams_sidebar.cpp:767`) selects the op type and builds the bar from `toolchange_phase_template`; this task supplies the live index.

**Files:**
- Modify: `include/ams_backend_happy_hare.h` (add private helper decl)
- Modify: `src/printer/ams_backend_happy_hare.cpp` (`parse_mmu_state` action block + helper)
- Test: `tests/unit/test_ams_backend_happy_hare.cpp`

**Interfaces:**
- Consumes: `AmsState::instance().get_active_step_operation()`, `AmsState::instance().set_narration_phase(int, const std::string&)`, `AmsState::instance().get_toolchange_step_subject()`; `lifetime_` (`helix::AsyncLifetimeGuard`); `toolchange_phase_template()` (Task 2); the existing `action`-parsing block in `parse_mmu_state()`.
- Produces: `void AmsBackendHappyHare::sync_narration_step()` (private) — maps `system_info_.action` → phase id → template index for the active op and defers `set_narration_phase`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "Happy Hare narration: action transitions advance the step subject",
                 "[ams][happy_hare][narration][ui_integration]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ams.set_active_step_operation(StepOperationType::LOAD_SWAP);
    ams.set_narration_phase(-1, "");

    auto hh = std::make_unique<AmsBackendHappyHareTestHelper>();
    hh->initialize_test_gates(4);
    auto* hh_raw = hh.get();
    ams.set_backend(std::move(hh));  // backend now reachable for the index subject

    auto feed_action = [&](const char* action) {
        nlohmann::json mmu; mmu["action"] = action;
        hh_raw->test_parse_mmu_state(mmu);
        helix::ui::UpdateQueue::instance().drain();  // flush the deferred set
    };

    feed_action("Heating");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 0);   // "heat" = index 0

    feed_action("Loading");
    // "feed" is index 5 in the LOAD_SWAP template.
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 5);

    feed_action("Purging");
    CHECK(lv_subject_get_int(ams.get_toolchange_step_subject()) == 6); // "purge" = index 6

    ams.set_backend(nullptr);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/bin/helix-tests "[happy_hare][narration][ui_integration]"`
Expected: FAIL — `parse_mmu_state` doesn't update the step subject yet; it stays at -1.

- [ ] **Step 3: Add the helper declaration**

In `include/ams_backend_happy_hare.h` private section:

```cpp
    // Synthesize a toolchange step index from the current AmsAction and push it
    // to AmsState's step subject (deferred to the main thread). Happy Hare emits
    // no // narration, so the backend drives the bar itself. Caller holds mutex_.
    void sync_narration_step();
```

- [ ] **Step 4: Implement the helper and call it from `parse_mmu_state`**

Add the helper (note the bg→main deferral via `lifetime_.token()`):

```cpp
void AmsBackendHappyHare::sync_narration_step() {
    // Caller holds mutex_. Map the current action to a phase id.
    const char* phase_id = nullptr;
    switch (system_info_.action) {
    case AmsAction::HEATING:     phase_id = "heat";     break;
    case AmsAction::FORMING_TIP: phase_id = "form_tip"; break;
    case AmsAction::CUTTING:     phase_id = "cut";      break;
    case AmsAction::UNLOADING:   phase_id = "unload";   break;
    case AmsAction::SELECTING:   phase_id = "select";   break;
    case AmsAction::LOADING:     phase_id = "feed";     break;
    case AmsAction::PURGING:     phase_id = "purge";    break;
    default: break;  // IDLE / CHECKING / ERROR / etc. → no step movement
    }
    if (!phase_id) return;

    const auto op = AmsState::instance().get_active_step_operation();
    const auto tmpl = toolchange_phase_template(op);
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == phase_id) {
            auto tok = lifetime_.token();
            const int index = static_cast<int>(k);
            std::string label = tmpl[k].label;
            tok.defer("AmsBackendHappyHare::sync_narration_step",
                      [index, label = std::move(label)]() {
                          AmsState::instance().set_narration_phase(index, label);
                      });
            return;
        }
    }
}
```

In `parse_mmu_state()`, at the END of the existing `if (mmu_data.contains("action") ...)` block (after `system_info_.action` is set), add:

```cpp
        // Drive the toolchange step bar from the action transition (HH has no
        // // narration). Deferred to main thread inside the helper.
        sync_narration_step();
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./build/bin/helix-tests "[happy_hare][narration][ui_integration]"`
Expected: PASS (step subject advances 0 → 5 → 6).

- [ ] **Step 6: Run the full AMS + error-center + narration suites for regressions**

Run: `make test-run && ./build/bin/helix-tests "[ams],[error-center],[narration]"`
Expected: PASS, including the existing AFC and narration-router tests (proving the shared layer is untouched).

- [ ] **Step 7: Commit**

```bash
git add include/ams_backend_happy_hare.h src/printer/ams_backend_happy_hare.cpp \
        tests/unit/test_ams_backend_happy_hare.cpp
git commit -m "feat(error-recovery): Happy Hare toolchange narration synthesis (ships blind)"
```

---

### Task 4: Router integration smoke test (no production code)

Confirms an HH-classified `!!` line flows through the shared `GcodeErrorRouter` to a recovery modal, and that no backend-type check was added to shared code.

**Files:**
- Test: `tests/unit/test_gcode_error_routing_e2e.cpp` (existing; add an HH case mirroring the AFC case)

**Interfaces:**
- Consumes: `AmsState::instance().set_backend(std::make_unique<AmsBackendHappyHare>(nullptr, nullptr))`, the existing routing-e2e fixture and its assertions on `decide_presentation` / produced `PromptData`.

- [ ] **Step 1: Write the test** (mirror the AFC routing-e2e case; install the HH backend, feed a paused `!!` runout line, assert `PresentAs::MODAL_WITH_RECOVER` and that the prompt has the Resume button).

```cpp
TEST_CASE_METHOD(LVGLTestFixture,
                 "Routing E2E: Happy Hare runout pause routes to recovery modal",
                 "[error-center][routing][happy_hare]") {
    auto hh = std::make_unique<AmsBackendHappyHare>(nullptr, nullptr);
    // Put HH into a runout error state via a status push, then route a !! line.
    // (Use the same envelope shape as AmsBackendHappyHareTestHelper::test_parse_mmu_state.)
    // ... install backend, feed status, build ClassifyContext{is_paused=true} ...
    // auto ev = AmsState::instance().get_backend()->classify_error("!! Runout detected", ctx);
    // REQUIRE(ev); CHECK(helix::decide_presentation(*ev) == helix::PresentAs::MODAL_WITH_RECOVER);
}
```

(Fill the elided lines from the existing AFC case in the same file — repeat its structure with the HH backend.)

- [ ] **Step 2: Run, verify pass**

Run: `./build/bin/helix-tests "[error-center][routing][happy_hare]"`
Expected: PASS.

- [ ] **Step 3: Abstraction gate — grep for leaks**

Run:
```bash
grep -rnE "AmsType::HAPPY_HARE|ErrorSource::HAPPY_HARE" src/ui/ src/application/gcode_error_router.cpp src/application/gcode_narration_router.cpp
```
Expected: NO matches in shared UI/router code (the `ErrorSource::HAPPY_HARE` tag is set inside the backend `.cpp` only). If any appear, that's a leak — refactor behind a virtual.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_gcode_error_routing_e2e.cpp
git commit -m "test(error-recovery): Happy Hare error routing e2e + abstraction gate"
```

---

## Self-Review

**Spec coverage (Phase 1 of the spec):**
- classify_error + recovery actions → Task 1 ✓
- toolchange_phase_template → Task 2 ✓
- narration synthesis from `action` → Task 3 ✓
- shared layer untouched / abstraction gate → Task 4 (grep gate) ✓
- Phase 0 unification → confirmed no-op in Global Constraints (base class already routes the sink) ✓

**Out of scope for this plan (later plans):** U1 (Phase 2), IFS (Phase 3), QIDI (Phase 4), #1052 + #1057 fold-ins (Phase 5). #1057 reference material is in `.claude/scratchpad/op-status-ref-test.cpp`.

**Type consistency:** `classify_error`/`build_recovery_actions`/`toolchange_phase_template`/`sync_narration_step` signatures are consistent across tasks. Phase ids in Task 2's template (`heat`/`form_tip`/`cut`/`unload`/`select`/`feed`/`purge`/`load`) exactly match the switch in Task 3's `sync_narration_step`. Index expectations in the Task 3 test (heat=0, feed=5, purge=6) match the LOAD_SWAP template order in Task 2.

**Ships-blind caveat:** the action→phase mapping and recovery gcode are best-effort from upstream docs/issues; verify and refine when Happy Hare hardware is available. Every commit message carries `(ships blind)`.

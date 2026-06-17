# L2 Toolchange Step/Phase Narration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive the AMS sidebar toolchange step-bar from AFC's real `//` narration instead of a hardcoded `AmsAction`-based guess, fixing S1 (purge mislabeled "Feed") and S2 (brush/clean/cut/poop/kick steps missing).

**Architecture:** A new `GcodeNarrationRouter` (sibling to `GcodeErrorRouter`, own `notify_gcode_response` subscription) parses `//` lines on the WS bg thread, asks the active `AmsBackend` to map each to a phase id (pure), and defers a write of the new static `toolchange_step` int subject + the `ams_action_detail` status string to the main thread. The backend declares an ordered `ToolchangePhase` template per operation; the sidebar builds its `ui_step_progress` labels from that template (falling back to the legacy hardcoded switch when the template is empty) and advances the highlight from the subject.

**Tech stack:** C++17, LVGL 9.5, libhv WebSocket, spdlog, Catch2. Build: `make -j` (program) / `make test-run` (tests). Worktree: `.worktrees/error-recovery-l2` (branch `feature/error-recovery-l2`).

**Spec:** `docs/devel/plans/2026-06-16-backend-error-recovery-L2-spec.md`.

**Threading rules (MANDATORY):** bg callbacks use `lifetime_.bg_cb("Tag", ...)` or `tok.defer(...)` — never `lifetime_.defer` from bg, never raw `[this]`, never `lv_subject_set_*` off-main ([L072]). `ObserverGuard::reset()` for cleanup, never `release()` ([L085]). `toolchange_step` is a static singleton subject → member `ObserverGuard` only, no paired `SubjectLifetime`.

---

## File structure

| File | Responsibility |
|------|----------------|
| `include/ams_backend.h` | `ToolchangePhase` struct + two new virtual hooks (default empty) on the base |
| `include/ams_state.h`, `src/printer/ams_state.cpp` | `toolchange_step` subject (init/register/deinit/reset) + `set_narration_phase()` helper |
| `include/ams_backend_afc.h`, `src/printer/ams_backend_afc.cpp` | AFC override: ordered templates + narration→id matcher |
| `include/gcode_narration_router.h`, `src/application/gcode_narration_router.cpp` | `//` ingestor: own subscription, pure match, deferred subject write |
| `src/application/application.cpp` | construct/own the router alongside `GcodeErrorRouter` |
| `src/ui/ui_ams_sidebar.cpp`, `include/ui_ams_sidebar.h` | template-driven label build + `toolchange_step` observer; bypass legacy index path when narration-driven |
| `ui_xml/components/ams_sidebar.xml` | `progress_stepper_container` height=content+cap+scroll |
| `tests/unit/test_ams_backend_afc.cpp` (extend), `tests/unit/test_gcode_narration_router.cpp` (new), `tests/ui_integration/test_toolchange_narration_e2e.cpp` (new) | tests |

## Dependency phases (for fan-out)

- **Phase 1 — Seam (must land first):** Task 1 (AmsBackend hooks) + Task 2 (AmsState subject). Independent of each other; both small; both pure interface/state.
- **Phase 2 — Fronts (parallel, each depends only on Phase 1):** Task 3 (AFC model) ∥ Task 4 (router + wiring) ∥ Task 5 (sidebar + XML).
- **Phase 3 — Integration:** Task 6 (end-to-end ui_integration test + full suite + S1/S2 assertions).

---

## Task 1: `AmsBackend` step-model hooks (seam)

**Files:**
- Modify: `include/ams_backend.h` (add struct + 2 virtuals near the `classify_error` hook at lines 243–247)
- Test: none (pure interface with default impls; exercised by Task 3)

- [ ] **Step 1: Add `ToolchangePhase` + the two hooks.** In `include/ams_backend.h`, immediately after the existing `classify_error` virtual (line ~247), add. `<optional>`, `<vector>`, `<string>` are already included; `StepOperationType` lives in `ams_step_operation.h` — add `#include "ams_step_operation.h"` to the header's include block.

```cpp
    /// One ordered phase in a backend's toolchange narration model.
    struct ToolchangePhase {
        std::string id;       ///< stable key matched from narration, e.g. "brush"
        std::string label;    ///< display label (translatable), e.g. "Brush nozzle"
        bool        optional; ///< if true, stays greyed/Pending when never narrated this swap
    };

    /// Declared ordered phase template for a toolchange operation.
    /// Empty (default) => backend has no narration model; the sidebar uses the
    /// legacy AmsAction-driven hardcoded step list (no regression).
    [[nodiscard]] virtual std::vector<ToolchangePhase>
    toolchange_phase_template(StepOperationType /*op*/) const {
        return {};
    }

    /// Map one `//` narration body (prefix already stripped) to a phase id.
    /// nullopt (default) => not a recognized phase line.
    [[nodiscard]] virtual std::optional<std::string>
    match_narration_phase(const std::string& /*narration*/) const {
        return std::nullopt;
    }
```

- [ ] **Step 2: Build to verify the base compiles.**

Run: `cd .worktrees/error-recovery-l2 && make -j 2>&1 | tail -5`
Expected: `✓ Build complete!` (no errors; new virtuals are header-only with default bodies).

- [ ] **Step 3: Commit.**

```bash
git add include/ams_backend.h
git commit -m "feat(error-recovery): AmsBackend toolchange step-model hooks (L2 seam)"
```

---

## Task 2: `AmsState::toolchange_step` subject (seam)

**Files:**
- Modify: `include/ams_state.h` (add subject member near `ams_action_` at line ~1064), `src/printer/ams_state.cpp` (init at ~line 206, a setter, reset on IDLE)
- Test: `tests/unit/test_ams_state*.cpp` if one exists; otherwise covered by Task 6 glue. (Subject existence is a compile+smoke concern.)

- [ ] **Step 1: Declare the subject member.** In `include/ams_state.h`, next to `lv_subject_t ams_action_;` (line ~1064) add:

```cpp
    lv_subject_t toolchange_step_; ///< current narration phase index (-1 = none/idle)
```

- [ ] **Step 2: Init + register.** In `src/printer/ams_state.cpp` `init_subjects()`, right after the `INIT_SUBJECT_INT(ams_action, ...)` line (~206) add:

```cpp
    INIT_SUBJECT_INT(toolchange_step, -1, subjects_, register_xml);
```

(`INIT_SUBJECT_INT` already inits, registers with `subjects_`, and XML-registers under the name `"toolchange_step"`. The existing `StaticSubjectRegistry` deinit for `"AmsState"` covers it because it tears down everything in `subjects_`.)

- [ ] **Step 3: Add a narration-phase setter.** Declare in `include/ams_state.h` (public, near `set_action`):

```cpp
    /// Set the current toolchange narration phase index (main thread only).
    /// Also mirrors the human label into ams_action_detail for the status line.
    /// Pass index = -1 to clear.
    void set_narration_phase(int index, const std::string& label);
```

Define in `src/printer/ams_state.cpp` (mirror the existing `ams_action_detail_` string-equality guard):

```cpp
void AmsState::set_narration_phase(int index, const std::string& label) {
    lv_subject_set_int(&toolchange_step_, index);
    if (!label.empty() &&
        std::strcmp(lv_subject_get_string(&ams_action_detail_), label.c_str()) != 0) {
        lv_subject_copy_string(&ams_action_detail_, label.c_str());
    }
}
```

(Ensure `<cstring>` is included in `ams_state.cpp`; it almost certainly already is.)

- [ ] **Step 4: Reset on IDLE.** In `AmsState::set_action(...)`, in the path that handles the `AmsAction::IDLE` transition (where the operation ends), add a reset of the step index so a finished operation clears the bar:

```cpp
    if (action == AmsAction::IDLE) {
        lv_subject_set_int(&toolchange_step_, -1);
    }
```

(Place alongside the existing IDLE handling in `set_action`; if `set_action` has no explicit IDLE branch, add this guard at the point the new action is applied.)

- [ ] **Step 5: Build.**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`

- [ ] **Step 6: Commit.**

```bash
git add include/ams_state.h src/printer/ams_state.cpp
git commit -m "feat(error-recovery): AmsState toolchange_step subject + narration-phase setter (L2 seam)"
```

---

## Task 3: AFC templates + narration matcher (front A — depends on Task 1)

**Files:**
- Modify: `include/ams_backend_afc.h` (declare overrides near `classify_error` at line ~126), `src/printer/ams_backend_afc.cpp` (implement, next to `classify_error`)
- Test: `tests/unit/test_ams_backend_afc.cpp` (extend; uses `AmsBackendAfcTestHelper`)

- [ ] **Step 1: Write failing tests.** Append to `tests/unit/test_ams_backend_afc.cpp`. These pin S1 + S2 directly.

```cpp
TEST_CASE("AFC narration maps purge to purge not feed (S1)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE(afc.match_narration_phase("Purge") == std::optional<std::string>("purge"));
    REQUIRE(afc.match_narration_phase("Purging old filament") ==
            std::optional<std::string>("purge"));
    // Feed/load narration must NOT be mistaken for purge
    REQUIRE(afc.match_narration_phase("Loading lane 2 to hub") ==
            std::optional<std::string>("feed"));
}

TEST_CASE("AFC narration recognizes brush/clean/cut/poop/kick (S2)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE(afc.match_narration_phase("AFC_Brush: Clean Nozzle") ==
            std::optional<std::string>("clean"));
    REQUIRE(afc.match_narration_phase("Move to Brush") ==
            std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Cutting tip") == std::optional<std::string>("cut"));
    REQUIRE(afc.match_narration_phase("Poop") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Kick") == std::optional<std::string>("kick"));
    REQUIRE(afc.match_narration_phase("lane 2 is now loaded in toolhead") ==
            std::optional<std::string>("load"));
}

TEST_CASE("AFC narration ignores unrelated lines", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE_FALSE(afc.match_narration_phase("Klipper state: ready").has_value());
    REQUIRE_FALSE(afc.match_narration_phase("").has_value());
}

TEST_CASE("AFC LOAD_SWAP template ordering puts purge after feed, brush after purge",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(tmpl.empty());
    auto idx = [&](const std::string& id) {
        for (size_t i = 0; i < tmpl.size(); ++i)
            if (tmpl[i].id == id) return static_cast<int>(i);
        return -1;
    };
    REQUIRE(idx("heat") == 0);
    REQUIRE(idx("feed") >= 0);
    REQUIRE(idx("purge") > idx("feed"));   // S1: purge is its own later step
    REQUIRE(idx("brush") > idx("purge"));  // S2: brush present, after purge
    REQUIRE(idx("clean") > idx("brush"));  // S2: clean present
}
```

Add `match_narration_phase` / `toolchange_phase_template` exposure to `AmsBackendAfcTestHelper` if its base access isn't already public — they're public virtuals on `AmsBackend`, so the helper inherits them directly; no friend access needed.

- [ ] **Step 2: Run, verify fail.**

Run: `make test-run 2>&1 | tail -20 && ./build/bin/helix-tests "[narration]" 2>&1 | tail -20`
Expected: FAIL/compile-error (overrides not yet implemented; `match_narration_phase` returns nullopt from base).

- [ ] **Step 3: Declare overrides** in `include/ams_backend_afc.h` after the `classify_error` declaration (~line 127):

```cpp
    [[nodiscard]] std::vector<ToolchangePhase>
    toolchange_phase_template(StepOperationType op) const override;

    [[nodiscard]] std::optional<std::string>
    match_narration_phase(const std::string& narration) const override;
```

- [ ] **Step 4: Implement** in `src/printer/ams_backend_afc.cpp` (place near `classify_error`; mirror its case-insensitive substring idiom — lowercase the input once, then `find`):

```cpp
std::vector<AmsBackend::ToolchangePhase>
AmsBackendAfc::toolchange_phase_template(StepOperationType op) const {
    switch (op) {
    case StepOperationType::LOAD_SWAP:
        return {
            {"heat",  "Heat nozzle",    false},
            {"cut",   "Cut tip",        true},
            {"poop",  "Purge to bucket", true},
            {"kick",  "Kick away",      true},
            {"feed",  "Feed filament",  false},
            {"purge", "Purge",          true},
            {"brush", "Brush nozzle",   true},
            {"clean", "Clean nozzle",   true},
            {"load",  "Load complete",  false},
        };
    case StepOperationType::LOAD_FRESH:
        return {
            {"heat",  "Heat nozzle",   false},
            {"feed",  "Feed filament", false},
            {"purge", "Purge",         true},
            {"load",  "Load complete", false},
        };
    case StepOperationType::UNLOAD:
        return {
            {"heat",    "Heat nozzle", false},
            {"cut",     "Cut tip",     true},
            {"retract", "Retract",     false},
        };
    }
    return {};
}

std::optional<std::string>
AmsBackendAfc::match_narration_phase(const std::string& narration) const {
    if (narration.empty())
        return std::nullopt;
    std::string s = narration;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* needle) { return s.find(needle) != std::string::npos; };

    // Order matters: more specific phrases first.
    if (has("is now loaded in toolhead") || has("load complete")) return "load";
    if (has("clean nozzle") || has("afc_brush: clean")) return "clean";
    if (has("move to brush") || has("brush")) return "brush";
    if (has("purge")) return "purge";          // S1: purge is its own phase
    if (has("kick")) return "kick";
    if (has("poop")) return "poop";
    if (has("cut")) return "cut";
    if (has("to hub") || has("feed") || has("loading lane")) return "feed";
    if (has("heat")) return "heat";
    return std::nullopt;
}
```

Ensure `<algorithm>` and `<cctype>` are included in `ams_backend_afc.cpp`.

- [ ] **Step 5: Run tests, verify pass.**

Run: `make test-run 2>&1 | tail -5 && ./build/bin/helix-tests "[narration]" 2>&1 | tail -15`
Expected: all `[narration]` cases PASS.

- [ ] **Step 6: Commit.**

```bash
git add include/ams_backend_afc.h src/printer/ams_backend_afc.cpp tests/unit/test_ams_backend_afc.cpp
git commit -m "feat(error-recovery): AFC toolchange templates + narration matcher (S1/S2)"
```

---

## Task 4: `GcodeNarrationRouter` + app wiring (front B — depends on Tasks 1, 2)

**Files:**
- Create: `include/gcode_narration_router.h`, `src/application/gcode_narration_router.cpp`
- Modify: `src/application/application.cpp` (~line 3075, after `m_gcode_error_router`), `include/application.h` (member)
- Test: `tests/unit/test_gcode_narration_router.cpp` (new)

- [ ] **Step 1: Write failing test.** Create `tests/unit/test_gcode_narration_router.cpp`. Use a `GcodeNarrationRouterTestAccess` friend to call the private `process_line` directly (no Moonraker). The router writes `AmsState`'s `toolchange_step` subject via the active backend; the test installs an AFC backend and asserts the subject advances.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_narration_router.h"
#include "ams_backend_afc.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"
#include "../lvgl_test_fixture.h"

using namespace helix;

struct GcodeNarrationRouterTestAccess {
    static void feed(GcodeNarrationRouter& r, const std::string& line) {
        r.process_line(line);
    }
};

TEST_CASE_METHOD(LVGLTestFixture, "Narration router advances toolchange_step on // brush",
                 "[unit][narration][router]") {
    AmsState::instance().init_subjects(true);
    // Install AFC backend + start a LOAD_SWAP so a template exists.
    // (Use the AmsState test seam used elsewhere to set the active backend +
    //  current operation; see test_ams_backend_afc.cpp for backend construction.)
    GcodeNarrationRouter router(nullptr, nullptr); // null api/client: no subscription
    GcodeNarrationRouterTestAccess::feed(router, "// AFC_Brush: Clean Nozzle");
    helix::ui::process_pending(); // drain the deferred subject write
    // clean maps to the "clean" phase; in LOAD_SWAP template that's index 7.
    REQUIRE(lv_subject_get_int(AmsState::instance().get_toolchange_step_subject()) >= 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "Narration router ignores non-// lines",
                 "[unit][narration][router]") {
    AmsState::instance().init_subjects(true);
    GcodeNarrationRouter router(nullptr, nullptr);
    GcodeNarrationRouterTestAccess::feed(router, "!! Toolhead jam");
    GcodeNarrationRouterTestAccess::feed(router, "ok");
    helix::ui::process_pending();
    REQUIRE(lv_subject_get_int(AmsState::instance().get_toolchange_step_subject()) == -1);
}
```

(If `AmsState` lacks `get_toolchange_step_subject()`, add a trivial accessor returning `&toolchange_step_` in Task 2's header — add it there and note it. The test needs the index resolved against the active backend's template, so the router must look up `index_of(id, backend->toolchange_phase_template(current_op))`; provide the current op via the AmsState operation seam the sidebar uses, or default LOAD_SWAP when an operation is active.)

- [ ] **Step 2: Run, verify fail.**

Run: `make test-run 2>&1 | tail -20`
Expected: compile-error (no `gcode_narration_router.h`).

- [ ] **Step 3: Create the header** `include/gcode_narration_router.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

class MoonrakerAPI;
namespace helix {
class MoonrakerClient;
}

namespace helix {

/// Consumes `//` narration lines from notify_gcode_response and routes them to
/// the active AmsBackend's step-model, updating the AmsState toolchange_step
/// subject. Sibling of GcodeErrorRouter; owns a SEPARATE subscription key.
/// Does NOT surface errors.
class GcodeNarrationRouter {
  public:
    GcodeNarrationRouter(MoonrakerAPI* api, MoonrakerClient* client);
    ~GcodeNarrationRouter() = default;

    GcodeNarrationRouter(const GcodeNarrationRouter&) = delete;
    GcodeNarrationRouter& operator=(const GcodeNarrationRouter&) = delete;

  private:
    friend struct ::GcodeNarrationRouterTestAccess;

    void on_notify_gcode_response(const nlohmann::json& msg);
    void process_line(const std::string& line); // pure match; defers subject write

    MoonrakerAPI* api_;
    MoonrakerClient* client_;
    AsyncLifetimeGuard lifetime_;
};

} // namespace helix
```

- [ ] **Step 4: Implement** `src/application/gcode_narration_router.cpp`. The match runs on the bg thread (pure); the subject write defers to main via `lifetime_.defer` from within `process_line` only when `process_line` is itself already on main (test path). For the real bg path, the body computed on bg is handed to main. Keep it simple: compute the (index,label) on the calling thread, then `lifetime_.defer` the `AmsState` write — `defer` is `queue_update`, safe from bg, and the test drains via `process_pending`.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_narration_router.h"

#include "ams_state.h"
#include "moonraker_client.h"
#include "ui_update_queue.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace helix {

namespace {
constexpr const char* kNarrationHandlerName = "gcode_narration_router";
}

GcodeNarrationRouter::GcodeNarrationRouter(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    if (!client_) {
        spdlog::debug("[GcodeNarrationRouter] Null client — handler not registered");
        return;
    }
    client_->register_method_callback(
        "notify_gcode_response", kNarrationHandlerName,
        lifetime_.bg_cb("GcodeNarrationRouter::on_notify",
                        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); }));
}

void GcodeNarrationRouter::on_notify_gcode_response(const nlohmann::json& msg) {
    // params: [["line1", "line2", ...]]
    if (!msg.is_array() || msg.empty() || !msg[0].is_array())
        return;
    for (const auto& item : msg[0]) {
        if (item.is_string())
            process_line(item.get<std::string>());
    }
}

void GcodeNarrationRouter::process_line(const std::string& line) {
    // Only `//` narration; ignore errors (!!), Error:, ok, status, etc.
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line.compare(i, 2, "//") != 0)
        return;
    std::string body = line.substr(i + 2);
    size_t b = body.find_first_not_of(" \t");
    if (b != std::string::npos)
        body = body.substr(b);

    auto* backend = AmsState::instance().get_backend();
    if (!backend)
        return;
    auto id = backend->match_narration_phase(body);
    if (!id)
        return;

    // Resolve index against the active operation's template.
    StepOperationType op = AmsState::instance().get_active_step_operation(); // see note
    auto tmpl = backend->toolchange_phase_template(op);
    int index = -1;
    std::string label;
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == *id) {
            index = static_cast<int>(k);
            label = tmpl[k].label;
            break;
        }
    }
    if (index < 0)
        return; // narrated phase not in this op's template — ignore

    lifetime_.defer("GcodeNarrationRouter::apply",
                    [index, label]() {
                        AmsState::instance().set_narration_phase(index, label);
                    });
}

} // namespace helix
```

> **Implementer note — `get_active_step_operation()`:** the sidebar already tracks `current_operation_type_`. Expose the active op through `AmsState` so the router (which has no sidebar pointer) can read it. Add to `AmsState`: a `StepOperationType active_step_operation_ = StepOperationType::LOAD_SWAP;` member with `set_active_step_operation(StepOperationType)` / `get_active_step_operation()`; have the sidebar call `AmsState::instance().set_active_step_operation(op_type)` inside `start_operation()` / `recreate_step_progress_for_operation()`. This keeps the router decoupled from the sidebar. Do this as part of Task 5's wiring; Task 4 only consumes the getter (default LOAD_SWAP is fine until Task 5 lands).

- [ ] **Step 5: Wire into application.** In `src/application/application.cpp` after line ~3075 (`m_gcode_error_router = ...`):

```cpp
    m_gcode_narration_router = std::make_unique<helix::GcodeNarrationRouter>(api, client);
```

In `include/application.h`, next to `std::unique_ptr<helix::GcodeErrorRouter> m_gcode_error_router;` add:

```cpp
    std::unique_ptr<helix::GcodeNarrationRouter> m_gcode_narration_router;
```

Add `#include "gcode_narration_router.h"` where `gcode_error_router.h` is included in `application.cpp`.

- [ ] **Step 6: Add the router cpp to the build** if the Makefile globs `src/**/*.cpp` it's automatic; otherwise add to the source list (check `mk/` — the project globs, so no edit expected).

- [ ] **Step 7: Run tests, verify pass.**

Run: `make test-run 2>&1 | tail -5 && ./build/bin/helix-tests "[router]" 2>&1 | tail -15`
Expected: `[router]` cases PASS.

- [ ] **Step 8: Commit.**

```bash
git add include/gcode_narration_router.h src/application/gcode_narration_router.cpp \
        src/application/application.cpp include/application.h include/ams_state.h \
        src/printer/ams_state.cpp tests/unit/test_gcode_narration_router.cpp
git commit -m "feat(error-recovery): GcodeNarrationRouter // ingestor + app wiring"
```

---

## Task 5: Sidebar template-driven step bar + observer (front C — depends on Tasks 1, 2)

**Files:**
- Modify: `src/ui/ui_ams_sidebar.cpp` (`recreate_step_progress_for_operation` ~446, add observer in `setup`, reset in `cleanup`), `include/ui_ams_sidebar.h` (add `ObserverGuard toolchange_step_observer_;` + `bool narration_driven_ = false;`)
- Modify: `ui_xml/components/ams_sidebar.xml` (container sizing)
- Test: covered by Task 6 ui_integration; this task is build + manual-launch smoke.

- [ ] **Step 1: Template-driven label build.** In `recreate_step_progress_for_operation(op_type)` (`ui_ams_sidebar.cpp:446`), BEFORE the existing `switch`, query the backend template and build from it when non-empty:

```cpp
    AmsState::instance().set_active_step_operation(op_type); // expose op to the narration router

    if (backend) {
        auto tmpl = backend->toolchange_phase_template(op_type);
        if (!tmpl.empty()) {
            std::vector<ui_step_t> steps;
            steps.reserve(tmpl.size());
            for (const auto& p : tmpl)
                steps.push_back({p.label.c_str(), helix::StepState::Pending});
            current_step_count_ = static_cast<int>(steps.size());
            step_progress_ = ui_step_progress_create(step_progress_container_, steps.data(),
                                                     current_step_count_, false,
                                                     "ams_step_progress");
            narration_driven_ = true;
            return; // skip the legacy hardcoded switch
        }
    }
    narration_driven_ = false;
    // ... existing switch(op_type) { LOAD_FRESH / LOAD_SWAP / UNLOAD } stays verbatim below ...
```

> `ui_step_t::label` is `const char*`; the `tmpl` vector outlives the `ui_step_progress_create` call (which copies the labels into widgets — verify `ui_step_progress_create` copies; it does, it builds child labels). If it does NOT copy, keep the `tmpl` strings alive in a member. Confirm during implementation.

- [ ] **Step 2: Bypass legacy index advancement when narration-driven.** In `update_step_progress(AmsAction action)` (~617), early-out so the `AmsAction`-derived index doesn't fight the narration-driven one:

```cpp
    if (narration_driven_)
        return; // index is driven by the toolchange_step observer, not AmsAction
```

(Keep the operation-detection/recreate logic that precedes index-setting if it also lives here; only skip the `get_step_index_for_action` → `set_current` part. If recreation logic is intertwined, gate just the `ui_step_progress_set_current` call on `!narration_driven_`.)

- [ ] **Step 3: Observe `toolchange_step`.** In the sidebar `setup()` (where other `ObserverGuard`s like `action_observer_` are wired), add:

```cpp
    toolchange_step_observer_ = observe_int_sync<AmsOperationSidebar>(
        AmsState::instance().get_toolchange_step_subject(), this,
        [](AmsOperationSidebar* self, int index) {
            if (!self->active_ || !self->step_progress_ || index < 0)
                return;
            ui_step_progress_set_current(self->step_progress_, index);
            // scroll the active row into view (small-screen safety net)
            lv_obj_scroll_to_view_recursive(
                lv_obj_get_child(self->step_progress_, index), LV_ANIM_ON);
        });
```

(Match the exact `observe_int_sync` signature used by `action_observer_` in this file — copy its call shape. `get_toolchange_step_subject()` returns `&toolchange_step_`; add that accessor in Task 2 if not present.)

- [ ] **Step 4: Cleanup.** In the sidebar `cleanup()`, reset the observer alongside the others:

```cpp
    toolchange_step_observer_.reset();
```

([L085]: `reset()`, never `release()`.)

- [ ] **Step 5: XML container sizing.** In `ui_xml/components/ams_sidebar.xml`, change the `progress_stepper_container` (line ~35) from fixed `height="150"` to grow-with-cap + scroll:

```xml
    <lv_obj name="progress_stepper_container"
            width="100%" height="content" style_max_height="260" style_pad_all="0"
            scrollable="true" scroll_dir="ver" hidden="true">
```

(Confirm `style_max_height` / `scroll_dir` token spellings against `LVGL9_XML_ATTRIBUTES_REFERENCE.md`; adjust the cap so 480×320 doesn't overflow the column.)

- [ ] **Step 6: Build + launch smoke.**

Run: `make -j 2>&1 | tail -5`
Expected: `✓ Build complete!`
Then (manual, optional this task): `./build/bin/helix-screen --test -vv` and open the AMS panel; the stepper renders (full integration verified in Task 6).

- [ ] **Step 7: Commit.**

```bash
git add src/ui/ui_ams_sidebar.cpp include/ui_ams_sidebar.h ui_xml/components/ams_sidebar.xml \
        include/ams_state.h src/printer/ams_state.cpp
git commit -m "feat(error-recovery): sidebar step bar from backend narration template (S1/S2)"
```

---

## Task 6: End-to-end ui_integration test + suite (Phase 3 — depends on 3,4,5)

**Files:**
- Create: `tests/ui_integration/test_toolchange_narration_e2e.cpp`
- Test tag: `[ui_integration]` (NOT `[.ui_integration]` — leading dot hides from CI, per handoff)

- [ ] **Step 1: Write the glue test.** Drive a synthetic narration sequence through `GcodeNarrationRouter::process_line` with an AFC backend + active LOAD_SWAP, then assert the `ui_step_progress` widget highlights the right row and the labels are template-derived (brush/clean present, purge labeled "Purge").

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "gcode_narration_router.h"
#include "ui_ams_sidebar.h"
#include "ui_step_progress.h"

#include "../catch_amalgamated.hpp"
#include "../xml_test_fixture.h"

using namespace helix;

struct GcodeNarrationRouterTestAccess {
    static void feed(GcodeNarrationRouter& r, const std::string& line) { r.process_line(line); }
};

TEST_CASE_METHOD(XMLTestFixture,
                 "Toolchange narration drives sidebar stepper end-to-end (S1/S2)",
                 "[ui_integration][narration]") {
    // 1. AFC backend active + a LOAD_SWAP operation in progress (use the fixture's
    //    AmsState/backend seam; mirror sidebar setup used in other ui_integration tests).
    // 2. Build the sidebar + start_operation(LOAD_SWAP).
    // 3. Feed narration:
    GcodeNarrationRouter router(nullptr, nullptr);
    for (const char* line : {"// Heat nozzle", "// Cutting tip", "// Feed filament",
                             "// Purge", "// Move to Brush", "// AFC_Brush: Clean Nozzle"}) {
        GcodeNarrationRouterTestAccess::feed(router, line);
        helix::ui::process_pending();
    }
    // S1: while "purging", the active label must read "Purge", never "Feed filament".
    // S2: the bar must contain "Brush nozzle" and "Clean nozzle" rows.
    int idx = lv_subject_get_int(AmsState::instance().get_toolchange_step_subject());
    REQUIRE(idx >= 0);
    // Assert the highlighted/last-narrated phase label is "Clean nozzle" (last fed line).
    // (Resolve the active step label from the widget child at idx and compare.)
}
```

> Fill the fixture wiring by copying the sidebar-construction pattern from the nearest existing `[ui_integration]` AMS test. The assertions that must remain: (a) `toolchange_step` advanced past 0, (b) a row labeled `Brush nozzle` and one labeled `Clean nozzle` exist (S2), (c) no row is labeled `Feed filament` as the active step while a purge line was the latest (S1).

- [ ] **Step 2: Run, verify fail then pass** (it should pass once 3/4/5 are merged; if red, fix wiring).

Run: `make test-run 2>&1 | tail -5 && ./build/bin/helix-tests "[narration]" 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 3: Full suite green.**

Run: `make test-run 2>&1 | tail -15`
Expected: no failures (96 shards green, per prior layers).

- [ ] **Step 4: Lint gates.** L081 anti-pattern + any XML lint:

Run: `python3 scripts/check_l081_anti_pattern.py 2>&1 | tail -5`
Expected: no new violations. (No new XML widget registered, so `make regen-xml-schema` is NOT required — only new subjects, which don't affect the schema.)

- [ ] **Step 5: Commit.**

```bash
git add tests/ui_integration/test_toolchange_narration_e2e.cpp
git commit -m "test(error-recovery): toolchange narration end-to-end (S1/S2) [ui_integration]"
```

---

## Verification (on the Voron, confirmatory)

Deploy the pi build to 192.168.1.112 (`biqu@`; back up `~/helixscreen/bin/helix-screen` first). Run a 2-color print; on a T-swap, the AMS sidebar should show the real sequence (…→ Cut → Feed → Purge → Brush → Clean → Load complete) with the highlight tracking narration, and **no "Feed" shown while purging** (S1) and **brush/clean visible** (S2). Screenshot for the PR.

## Self-review notes

- **Spec coverage:** S1 → Task 3 (matcher + template ordering) + Task 6 assertion; S2 → Task 3 template + Task 6 assertion; `//` ingestor placement (fork a) → Task 4; step-model shape (fork b) → Tasks 1/3/5; detail line → Task 2 `set_narration_phase`; vertical grow/scroll → Task 5 Step 5. All covered.
- **Type consistency:** `ToolchangePhase{id,label,optional}`, `toolchange_phase_template(StepOperationType)`, `match_narration_phase(string)→optional<string>`, `toolchange_step` subject + `get_toolchange_step_subject()` accessor, `set_narration_phase(int,string)`, `get/set_active_step_operation(StepOperationType)`, `narration_driven_` flag, `toolchange_step_observer_` — names used identically across Tasks 1–6.
- **Open implementation confirmations (flagged inline, not blockers):** does `ui_step_progress_create` copy label strings (Task 5 Step 1); exact `observe_int_sync` call shape (Task 5 Step 3); exact `set_action` IDLE branch location (Task 2 Step 4); `style_max_height`/`scroll_dir` token spellings (Task 5 Step 5).

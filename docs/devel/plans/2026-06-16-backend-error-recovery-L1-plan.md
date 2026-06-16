# L1 — AFC Classification + Multi-Button Recovery Presenter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** AFC's toolhead jam (and lane/hub/any pausing AFC fault) surfaces a CRITICAL modal with smart, applicable recovery buttons (Resume / Unload / Eject / Recover), rendered by a generic presenter that turns any `ErrorEvent.recovery_actions[]` into buttons — replacing L0's single-button `find_recovery` special case.

**Architecture:** `AmsBackendAfc::classify_error()` matches the jam text (and falls back to a paused+`error_state_` catch-all), reads live toolhead state, and emits only the recovery actions that apply (hide-inapplicable, no precondition field). A pure `build_recovery_prompt(ErrorEvent)→PromptData` maps actions→buttons; the router owns a reusable `ActionPromptModal` to render them and runs the tapped action's gcode via `api_->execute_gcode`. `key840` folds into this same path, retiring `find_recovery`.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (amalgamated), `make test-run`. Spec: `docs/devel/plans/2026-06-16-backend-error-recovery-L1-spec.md`.

**Acceptance (callouts closed):** R1 (Unload offered instead of failing Eject when toolhead loaded), R3 (context-aware recovery actions), E3 (overlay sensor-render audit).

---

## File Structure

| File | Responsibility | Action |
|------|----------------|--------|
| `include/error_event.h` | Add `style` to `RecoveryAction` | Modify |
| `include/ams_backend_afc.h` | `classify_error()` override decl; `build_recovery_actions()` helper decl | Modify |
| `src/printer/ams_backend_afc.cpp` | Parse `AFC.error_state`; implement `classify_error()` + `build_recovery_actions()` | Modify |
| `tests/unit/test_ams_backend_afc.cpp` | classify_error unit tests | Modify (append) |
| `include/gcode_error_router.h` | `build_recovery_prompt()` decl; `recovery_modal_` + dedup members | Modify |
| `src/application/gcode_error_router.cpp` | `build_recovery_prompt()`; generic multi-button presenter; retire `find_recovery`/single-button `present_recovery_modal` | Modify |
| `tests/unit/test_gcode_error_routing.cpp` | `build_recovery_prompt` unit tests | Modify (append) |
| `docs/devel/FILAMENT_MANAGEMENT.md` or spec appendix | E3 audit finding | Modify/append (Task 6) |

---

## Task 1: Add `style` field to RecoveryAction

**Files:**
- Modify: `include/error_event.h`
- Test: `tests/unit/test_error_classify.cpp` (append)

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_error_classify.cpp`:

```cpp
TEST_CASE("RecoveryAction carries an optional style", "[error-center][model]") {
    helix::RecoveryAction a;
    REQUIRE(a.style.empty());                 // default neutral
    helix::RecoveryAction b{"Resume", "RESUME", "afc::resume", "primary"};
    REQUIRE(b.style == "primary");
    REQUIRE(b.gcode == "RESUME");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[error-center][model]" -v`
Expected: FAIL to compile — `RecoveryAction` has no 4th member `style` (the brace-init with 4 args is ill-formed).

- [ ] **Step 3: Write minimal implementation**

In `include/error_event.h`, change the `RecoveryAction` struct to:

```cpp
struct RecoveryAction {
    std::string label;   ///< Button label, e.g. "Unload"
    std::string gcode;   ///< G-code to run on tap
    std::string log_tag; ///< spdlog tag on tap
    std::string style;   ///< "" (neutral) | "primary" | "danger" — maps to PromptButton.color
};
```

> Note: existing aggregate inits like `{"Reset CFS", "BOX_ERROR_CLEAR", "tag"}` remain valid — the 4th member defaults to `""`. No other edits required.

- [ ] **Step 4: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[error-center][model]" -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/error_event.h tests/unit/test_error_classify.cpp
git commit -m "feat(error-center): add optional style to RecoveryAction"
```

---

## Task 2: AmsBackendAfc::classify_error() + error_state parsing + recovery-action builder

**Files:**
- Modify: `include/ams_backend_afc.h`, `src/printer/ams_backend_afc.cpp`
- Test: `tests/unit/test_ams_backend_afc.cpp` (append)

Context: `classify_error()` receives the raw `!!` line; the toolhead/error state comes from previously-fed status JSON. The jam text matches explicitly; anything else that pauses while `error_state_` is set is claimed by a catch-all. `build_recovery_actions()` reads live state and pushes only applicable actions. `error_state_` is declared (`ams_backend_afc.h:517`) but not currently parsed — this task adds the parse.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_ams_backend_afc.cpp`:

```cpp
// ---- L1: classify_error ----
static const char* kJamLine =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still "
    "detect filament. Possible filament break or jam at the toolhead. Please clear "
    "the jam and reload filament manually, then resume the print.";

TEST_CASE("AFC jam with toolhead loaded offers Unload not Eject", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder",
                             {{"tool_start_status", true}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos);  // full text

    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Unload"));   // toolhead loaded -> Unload (closes R1)
    REQUIRE(has("Recover"));
    REQUIRE_FALSE(has("Eject"));
}

TEST_CASE("AFC jam with empty toolhead offers Eject not Unload", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // tool_start_status=false (empty toolhead), but a lane is selected for eject.
    helper.feed_afc_extruder("extruder",
                             {{"tool_start_status", false}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Eject"));      // empty toolhead -> Eject
    REQUIRE_FALSE(has("Unload"));
    REQUIRE(has("Recover"));
}

TEST_CASE("AFC catch-all: paused + error_state + unknown !! is CRITICAL AFC",
          "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_state({{"error_state", true}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error("!! Some AFC fault we do not recognize", ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE_FALSE(e->recovery_actions.empty());  // std actions attached
}

TEST_CASE("AFC defers when not paused and no error_state", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;  // idle, no error_state fed
    REQUIRE_FALSE(helper.classify_error("!! Some AFC fault", ctx).has_value());
}

TEST_CASE("AFC ignores non-error lines", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    REQUIRE_FALSE(helper.classify_error("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(helper.classify_error("ok", ctx).has_value());
}
```

Ensure the test file includes `<algorithm>` (it already does, per line 13) and `"error_event.h"`. Add `#include "error_event.h"` near the other includes if not present.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[ams][afc][classify]" -v`
Expected: FAIL to compile — `AmsBackendAfc` has no `classify_error` override (base default is not virtual-overridden) AND `error_state` is not parsed (catch-all test would fail at runtime).

- [ ] **Step 3: Write minimal implementation**

**3a.** In `include/ams_backend_afc.h`, add `#include "error_event.h"` near the top includes if not already present. In the `public:` section (alongside the other query methods), declare the override:

```cpp
/// L1: recognize AFC toolhead jam / lane / hub faults and emit a CRITICAL
/// ErrorEvent with context-aware recovery actions. Falls back to a catch-all
/// for any pausing !! while error_state_ is set. Returns nullopt otherwise so
/// the generic classifier handles the line.
[[nodiscard]] std::optional<helix::ErrorEvent> classify_error(
    const std::string& raw_line, const helix::ClassifyContext& ctx) const override;
```

In the `private:` section, declare the builder:

```cpp
/// Build the applicable recovery actions for an AFC pause/jam, reading live
/// toolhead state. Caller holds mutex_. Hide-inapplicable: Unload only when
/// the toolhead is loaded; Eject only when empty and a lane is selected.
std::vector<helix::RecoveryAction> build_recovery_actions() const;
```

**3b.** In `src/printer/ams_backend_afc.cpp`, add the `error_state` parse inside `parse_afc_state()` (after the existing `message` block, ~line 792):

```cpp
    if (afc_data.contains("error_state") && afc_data["error_state"].is_boolean()) {
        error_state_ = afc_data["error_state"].get<bool>();
    }
```

**3c.** Implement the two methods (place near the other query methods; both `const`, both lock `mutex_`):

```cpp
std::vector<helix::RecoveryAction> AmsBackendAfc::build_recovery_actions() const {
    // Caller holds mutex_.
    std::vector<helix::RecoveryAction> actions;

    // Resume after the user clears the jam (always offered).
    actions.push_back({lv_tr("Resume"), "RESUME", "afc::resume", "primary"});

    const bool toolhead_loaded = tool_start_sensor_ || system_info_.filament_loaded;
    if (toolhead_loaded) {
        // Closes R1: unload from the toolhead before any eject is possible.
        actions.push_back({lv_tr("Unload"), "TOOL_UNLOAD", "afc::tool_unload", ""});
    } else if (!current_lane_name_.empty()) {
        // Empty toolhead but a lane is selected — eject that lane.
        actions.push_back({lv_tr("Eject"), "LANE_UNLOAD LANE=" + current_lane_name_,
                           "afc::lane_unload", ""});
    }

    // Reset/re-prep all lanes (last resort).
    actions.push_back({lv_tr("Recover"), "AFC_RESET", "afc::reset", "danger"});
    return actions;
}

std::optional<helix::ErrorEvent> AmsBackendAfc::classify_error(
    const std::string& raw_line, const helix::ClassifyContext& ctx) const {
    // Only `!!` emergency lines are candidates.
    if (raw_line.size() < 2 || raw_line[0] != '!' || raw_line[1] != '!') {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Strip the "!! " prefix for the detail text.
    std::string detail = (raw_line.size() > 3 && raw_line[2] == ' ') ? raw_line.substr(3)
                                                                     : raw_line.substr(2);

    auto contains_ci = [](const std::string& hay, const char* needle) {
        std::string h = hay;
        std::string n = needle;
        for (auto& c : h) c = static_cast<char>(std::tolower(c));
        for (auto& c : n) c = static_cast<char>(std::tolower(c));
        return h.find(n) != std::string::npos;
    };

    // 1) Toolhead jam / break (handle_toolhead_runout signature).
    const bool is_jam = contains_ci(detail, "tool_end") &&
                        (contains_ci(detail, "jam") || contains_ci(detail, "break") ||
                         contains_ci(detail, "runout detected"));
    if (is_jam) {
        helix::ErrorEvent e;
        e.source = helix::ErrorSource::AFC;
        e.severity = helix::ErrorSeverity::CRITICAL;
        e.title = lv_tr("Toolhead jam");
        e.detail = detail;
        e.sticky = true;
        e.recovery_actions = build_recovery_actions();
        return e;
    }

    // 2) Catch-all: any pausing !! while AFC is in an error state.
    if (ctx.is_paused && error_state_) {
        helix::ErrorEvent e;
        e.source = helix::ErrorSource::AFC;
        e.severity = helix::ErrorSeverity::CRITICAL;
        e.title = lv_tr("Filament system error");
        e.detail = detail;
        e.sticky = true;
        e.recovery_actions = build_recovery_actions();
        return e;
    }

    // 3) Not an AFC-owned fault — let the generic classifier handle it.
    return std::nullopt;
}
```

Ensure `<cctype>`, `<optional>`, and `<vector>` are included in the .cpp (add any missing). `mutex_` is the existing member used by `eject_lane`/`dispatch_lane_unload`; it is mutable for use in const methods (confirm: other const query methods lock it — if `mutex_` is not declared `mutable`, add `mutable` to its declaration in the header).

> **Resume macro note:** Use plain `RESUME` (always valid while paused). Quick check before finalizing: `grep -rniE 'AFC_RESUME|RESUME' lib/ ../AFC* 2>/dev/null` and the AFC docs comment block in `ams_backend_afc.h` (~lines 40-46) — if an AFC-specific resume macro is the documented entry point, substitute it. Default stays `RESUME`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[ams][afc][classify]" -v`
Expected: PASS (all 5 sections). Then `./build/bin/helix-tests "[ams][afc]" -v` — no regression in existing AFC tests.

- [ ] **Step 5: Commit**

```bash
git add include/ams_backend_afc.h src/printer/ams_backend_afc.cpp tests/unit/test_ams_backend_afc.cpp
git commit -m "feat(ams-afc): classify_error — jam + catch-all with context-aware recovery actions"
```

---

## Task 3: Pure build_recovery_prompt (ErrorEvent → PromptData)

**Files:**
- Modify: `include/gcode_error_router.h`, `src/application/gcode_error_router.cpp`
- Test: `tests/unit/test_gcode_error_routing.cpp` (append)

A pure function maps `recovery_actions[]` → `PromptData.buttons[]`, mapping `style` → `PromptButton.color` (`"primary"`→`"primary"`, `"danger"`→`"error"`, `""`→`""`). LVGL-free and unit-testable.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_gcode_error_routing.cpp`:

```cpp
#include "action_prompt_manager.h"  // helix::PromptData / helix::PromptButton

TEST_CASE("build_recovery_prompt maps actions to buttons", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "Possible filament break at the toolhead.";
    e.recovery_actions.push_back({"Resume", "RESUME", "afc::resume", "primary"});
    e.recovery_actions.push_back({"Unload", "TOOL_UNLOAD", "afc::tool_unload", ""});
    e.recovery_actions.push_back({"Recover", "AFC_RESET", "afc::reset", "danger"});

    helix::PromptData p = helix::build_recovery_prompt(e);

    REQUIRE(p.title == "Toolhead jam");
    REQUIRE(p.text_lines.size() == 1);
    REQUIRE(p.text_lines[0] == "Possible filament break at the toolhead.");
    REQUIRE(p.buttons.size() == 3);
    REQUIRE(p.buttons[0].label == "Resume");
    REQUIRE(p.buttons[0].gcode == "RESUME");
    REQUIRE(p.buttons[0].color == "primary");
    REQUIRE(p.buttons[1].label == "Unload");
    REQUIRE(p.buttons[1].color.empty());            // neutral
    REQUIRE(p.buttons[2].color == "error");          // "danger" -> "error"
}

TEST_CASE("build_recovery_prompt falls back to default title", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    e.detail = "x";
    e.recovery_actions.push_back({"Reset CFS", "BOX_ERROR_CLEAR", "t", ""});
    helix::PromptData p = helix::build_recovery_prompt(e);
    REQUIRE_FALSE(p.title.empty());                  // non-empty default title
    REQUIRE(p.buttons.size() == 1);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[error-center][routing]" -v`
Expected: FAIL to compile — `helix::build_recovery_prompt` undeclared.

- [ ] **Step 3: Write minimal implementation**

In `include/gcode_error_router.h`, add the include and declaration (namespace `helix`, near `decide_presentation`):

```cpp
#include "action_prompt_manager.h"  // PromptData / PromptButton
```
```cpp
/// Pure: turn an ErrorEvent's recovery actions into a renderable PromptData.
/// LVGL-free; style maps to PromptButton.color. Title falls back to a default.
PromptData build_recovery_prompt(const ErrorEvent& e);
```

In `src/application/gcode_error_router.cpp` (in `namespace helix`, near `decide_presentation`):

```cpp
namespace {
std::string color_for_style(const std::string& style) {
    if (style == "primary") return "primary";
    if (style == "danger") return "error";
    return "";  // neutral / theme default
}
}  // namespace

PromptData build_recovery_prompt(const ErrorEvent& e) {
    PromptData p;
    p.title = e.title.empty() ? std::string(lv_tr("Printer Error")) : e.title;
    if (!e.detail.empty()) p.text_lines.push_back(e.detail);
    for (const auto& a : e.recovery_actions) {
        PromptButton b;
        b.label = a.label;
        b.gcode = a.gcode;
        b.color = color_for_style(a.style);
        p.buttons.push_back(std::move(b));
    }
    return p;
}
```

> If linking the test pulls in LVGL via `lv_tr`, the test already links the LVGL test core (the routing tests build against the same target as the model tests). `lv_tr` returns the source string when no translation is loaded, so the default-title assertion holds.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[error-center][routing]" -v`
Expected: PASS (all sections, including the 5 pre-existing routing tests).

- [ ] **Step 5: Commit**

```bash
git add include/gcode_error_router.h src/application/gcode_error_router.cpp tests/unit/test_gcode_error_routing.cpp
git commit -m "feat(error-center): pure build_recovery_prompt (ErrorEvent -> PromptData)"
```

---

## Task 4: Generic multi-button presenter; retire find_recovery

**Files:**
- Modify: `include/gcode_error_router.h`, `src/application/gcode_error_router.cpp`

Wire the `MODAL_WITH_RECOVER` case to render `build_recovery_prompt(e)` through a router-owned `ActionPromptModal`, run the tapped action via `execute_gcode`, and retire the `CfsRecoveryEntry`/`find_recovery`/single-button `present_recovery_modal`. This is LVGL/main-thread code; correctness of the mapping is already covered by Task 3's unit tests, and the end-to-end behavior is verified on-device in Task 6. No new unit test in this task — verification is the compile + the device run.

- [ ] **Step 1: Header changes**

In `include/gcode_error_router.h`:
- Add include: `#include "action_prompt_modal.h"` (helix::ui::ActionPromptModal).
- Add `#include <memory>`.
- Change the signature of `present_recovery_modal` to keep the same name (still called from the switch). Add two private members **before** `AsyncLifetimeGuard lifetime_;` (so `lifetime_` remains the last-declared member and destructs first):

```cpp
    std::unique_ptr<helix::ui::ActionPromptModal> recovery_modal_;
    std::vector<RecoveryAction> active_recovery_actions_;  ///< actions for the in-flight modal
    std::string shown_recovery_detail_;                    ///< dedup: detail of the visible modal
```

(`<vector>` is already included.)

- [ ] **Step 2: Replace present_recovery_modal and delete the static table**

In `src/application/gcode_error_router.cpp`:

**2a.** Delete the `CfsRecoveryEntry` struct, the `RecoveryCtx` struct, and `find_recovery()` (~lines 44-73). Remove now-unused includes only if they become unused (keep `ui_toast_manager.h`, `moonraker_error.h`).

**2b.** Replace the entire body of `present_recovery_modal()` with:

```cpp
void GcodeErrorRouter::present_recovery_modal(const ErrorEvent& e) {
    if (!api_) {
        ui_notification_error(modal_title_for(e), e.detail.c_str(), /*modal=*/true);
        return;
    }

    // Dedup: the AFC jam repeats; don't re-pop the same modal.
    if (e.detail == shown_recovery_detail_) {
        spdlog::debug("[GcodeError] Skipping duplicate recovery modal: {}", e.detail);
        return;
    }

    if (!recovery_modal_) {
        recovery_modal_ = std::make_unique<helix::ui::ActionPromptModal>();
        recovery_modal_->set_gcode_callback([this](const std::string& gcode) {
            // Runs on the main thread (button tap). Find the action for its log_tag.
            std::string tag = "GcodeErrorRouter::recovery";
            for (const auto& a : active_recovery_actions_) {
                if (a.gcode == gcode) { tag = a.log_tag; break; }
            }
            shown_recovery_detail_.clear();  // user acted; allow re-show on a new fault
            spdlog::info("[GcodeError] User tapped recovery: {} ({})", tag, gcode);
            api_->execute_gcode(
                gcode,
                [tag]() { spdlog::info("[Recovery] {} completed", tag); },
                [tag](const MoonrakerError& err) {
                    spdlog::error("[Recovery] {} failed: {}", tag, err.message);
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        ("Recovery failed: " + err.user_message()).c_str(), 6000);
                },
                MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
        });
    }

    active_recovery_actions_ = e.recovery_actions;
    shown_recovery_detail_ = e.detail;

    lv_obj_t* screen = lv_screen_active();
    if (!screen || !recovery_modal_->show_prompt(screen, build_recovery_prompt(e))) {
        spdlog::warn("[GcodeError] recovery modal show failed; falling back to alert");
        shown_recovery_detail_.clear();
        ui_notification_error(modal_title_for(e), e.detail.c_str(), /*modal=*/true);
    }
}
```

> `key840` now flows here: `classify()` populates its `recovery_actions` (`{"Reset CFS","BOX_ERROR_CLEAR","error_classify::key840_reset"}`), so `decide_presentation` returns `MODAL_WITH_RECOVER` and this renders a one-button modal running `BOX_ERROR_CLEAR` — same gcode as before, now generic.

- [ ] **Step 3: Build and verify no regressions in the suite**

Run: `make test-run`
Expected: build succeeds; `[error-center]`, `[ams][afc]`, `[printer-state]` tags all pass. The retired `find_recovery` has no remaining references (grep to confirm):

Run: `grep -rn "find_recovery\|CfsRecoveryEntry\|RecoveryCtx" src/ include/`
Expected: no matches.

- [ ] **Step 4: Build the program binary**

Run: `make -j`
Expected: links clean (the router now owns an `ActionPromptModal`; confirm no missing-symbol/circular-include errors).

- [ ] **Step 5: Commit**

```bash
git add include/gcode_error_router.h src/application/gcode_error_router.cpp
git commit -m "feat(error-center): generic multi-button recovery modal via ActionPromptModal; retire find_recovery"
```

---

## Task 5: Mock-backed manual smoke (local, pre-device)

**Files:** none (runtime verification)

Validates the full path locally with the mock printer before the Voron run.

- [ ] **Step 1: Launch the mock with debug logs**

Run (background, tee to log):
```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/l1-smoke.log
```

- [ ] **Step 2: Inject a jam while "paused"**

If the mock supports a gcode-response injection path, emit the jam line; otherwise confirm via the unit tests + proceed to device. Document in the PR which path was used. Looked-for log line: `[GcodeError] sev=2 src=3 ...` (CRITICAL, source AFC) followed by the recovery modal showing.

- [ ] **Step 3: Read the log**

Run: `grep -nE "GcodeError|Recovery|recovery modal" /tmp/l1-smoke.log`
Expected: classification logged as CRITICAL/AFC; modal shown; no `present_recovery_modal show failed` warning.

> No commit — this is a verification gate. If the mock has no `!!` injection hook, note it and rely on Task 6 (device) for end-to-end confirmation.

---

## Task 6: E3 audit — AMS overlay sensor/path render during error

**Files:**
- Read: `src/ui/ui_panel_ams.cpp`, `src/ui/ui_ams_sidebar.cpp`, the filament-path canvas (`docs/devel/FILAMENT_PATH_CANVAS.md` names the files)
- Modify (only if a lag is found): the offending render path
- Modify (always): append the finding to `docs/devel/plans/2026-06-16-backend-error-recovery-L1-spec.md` (§ "E3 audit finding")

Spec hypothesis: the overlay reads live `AFC_stepper` sensor state, refreshed by `handle_status_update → EVENT_STATE_CHANGED` in the same status batch. Confirm or refute; do not silently drop the callout.

- [ ] **Step 1: Trace the sensor/path render source**

Run: `grep -rnE "EVENT_STATE_CHANGED|slots_version_observer|refresh_slots|path_filament_segment|sensors\.(prep|load|loaded_to_hub)" src/ui/ui_panel_ams.cpp src/ui/ui_ams_sidebar.cpp`
Determine: does the lane sensor / path render read `get_slot_info(i)->sensors.*` (or the path topology subject) on every `EVENT_STATE_CHANGED`, or from a source updated on a different/slower cadence than the AFC status batch?

- [ ] **Step 2: Decide**

- If the render reads live sensor state per status batch → **confirmed live, no fix.** Go to Step 4.
- If it lags (e.g. the per-slot path canvas only repaints on `path_filament_segment`/`path_topology` subject change, not per-lane status — the P2-adjacent symptom) → **fix:** route the per-lane sensor/segment refresh through the same `EVENT_STATE_CHANGED` signal the spool render already uses. Implement the minimal change in the offending file.

- [ ] **Step 3 (only if a fix was made): Add/extend a regression test**

If a code fix was made and the render logic has a unit-testable seam (a pure "does slot N show a triggered sensor?" function), add a Catch2 test asserting the sensor state mirrors `get_slot_info` after a status update. If the render is purely LVGL-side with no testable seam, document that device verification (Task 7) covers it.

- [ ] **Step 4: Document the finding**

Append a short "E3 audit finding" section to the L1 spec file stating: which file/observer drives the sensor/path render, whether it lagged, and what was done (no-op confirmation or the fix). Commit:

```bash
git add docs/devel/plans/2026-06-16-backend-error-recovery-L1-spec.md
# plus any code/test files if a fix was made
git commit -m "docs(error-recovery): E3 audit — AMS overlay sensor render finding"
```

---

## Task 7: On-device verification (Voron)

**Files:** none (device verification)

The Voron (192.168.1.112, biqu@) runs the L0 dev pi build. Deploy L1 and confirm the recovery modal.

- [ ] **Step 1: Build for Pi and deploy**

Per the device's deploy path (the L0 build was left at `~/helixscreen/bin/helix-screen`, rollback at `helix-screen.preL0.bak`). Back up the current binary before overwriting, then deploy the L1 pi build. Confirm the new binary is running (PID start time / version in the log) before interacting — [L080].

- [ ] **Step 2: Inject the jam while paused**

With a print paused (or pause first), in the Moonraker/Mainsail console:
```
RESPOND TYPE=error MSG="Toolhead runout detected by tool_end sensor, but upstream sensors still detect filament. Possible filament break or jam at the toolhead. Please clear the jam and reload filament manually, then resume the print."
```
(Or reproduce the real jam — it recurs readily.)

- [ ] **Step 3: Confirm the modal**

Expected: a CRITICAL modal titled *"Toolhead jam"* with the **full** message and buttons **Resume / Unload / Recover** (Unload present because the toolhead is loaded), NOT a dead-end OK. Screenshot for the PR. Verify in the log: `[GcodeError] sev=2 src=3 ...` and, on tapping a button, `[GcodeError] User tapped recovery: afc::...`.

- [ ] **Step 4: Regression check**

Confirm a CFS-style coded error (if testable) and a plain `Error:` line still behave as before (no regression). Confirm `key840` (if reproducible) shows a one-button "Reset CFS" modal running `BOX_ERROR_CLEAR`.

---

## Self-Review

- **Spec coverage:**
  - §4.1 `style` field → Task 1. ✓
  - §4.2 AFC `classify_error` (jam / lane-hub / catch-all / nullopt) + `build_recovery_actions` precondition table (Unload when loaded, Eject when empty) → Task 2. ✓ (lane/hub explicit matches subsumed by the jam matcher + catch-all per the "jam + AFC-paused catch-all" decision; enumerating extra lane/hub strings is optional and noted in spec §7.)
  - §4.3 pure `build_recovery_prompt` → Task 3; router-owned `ActionPromptModal`, execute_gcode + failure toast, retire `find_recovery`, key840 folds in → Task 4. ✓
  - §4.4 E3 audit (+ conditional fix + documented finding) → Task 6. ✓
  - §5 testing: AFC classify unit tests (Task 2), build_recovery_prompt unit tests (Task 3), device verify (Task 7), local smoke (Task 5). ✓
- **Placeholder scan:** No TBD/TODO. The one runtime conditional (Task 6 fix-or-confirm) has explicit decision criteria and a guaranteed documentation deliverable. The Resume-macro check (Task 2) has a concrete default (`RESUME`) and a grep to confirm. ✓
- **Type consistency:** `RecoveryAction{label,gcode,log_tag,style}` consistent across Tasks 1-4. `classify_error(const std::string&, const helix::ClassifyContext&) const` matches the base hook. `build_recovery_prompt(const ErrorEvent&) -> helix::PromptData`; `PromptButton{label,gcode,color}` mapping matches the struct (action_prompt_manager.h). `recovery_modal_` declared before `lifetime_`. `execute_gcode(gcode, success, error, AMS_OPERATION_TIMEOUT_MS)` matches the API signature. ✓
- **Threading:** `present_recovery_modal` and `build_recovery_prompt` run on the main thread (router `process_line` is deferred via `lifetime_.bg_cb`). `classify_error` locks `mutex_` (status applied off-main). No sync widget deletes introduced; `ActionPromptModal` manages its own token-guarded teardown. ✓

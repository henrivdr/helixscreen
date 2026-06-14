# Wizard Step Registry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the wizard's raw-`int` step indexing (45 branch sites, dual skip mechanism, no base class) with a `StepId`-named, registry-driven, per-step-skip architecture.

**Architecture:** A `helix::wizard::Step` abstract base; 13 existing step singletons inherit it and each owns `should_skip(const StepContext&)`. An ordered registry of `Step*` is the single source of order+identity. Pure, LVGL-free nav functions walk the registry. `ui_wizard.cpp` dispatches via polymorphism — no integer step math anywhere.

**Tech Stack:** C++17, LVGL 9.5, Catch2 (`tests/unit/`), `make test` / `./build/bin/helix-tests`.

**Reference:** spec at `docs/devel/plans/2026-06-14-wizard-step-registry.md` (skip table, decisions).

**Conventions:** TDD (failing test first). Build tests with `make test`; run a tag with `./build/bin/helix-tests "[tag]"`. Filter noise with `| grep -vE "info\]|locale:"`. Commit after each green task. Worktree: `.worktrees/wizard-step-registry`.

---

## File Structure

- `include/wizard_step.h` — **new**: `StepId` enum, `StepContext` struct, `Step` base class.
- `include/wizard_step_logic.h` + `src/system/wizard_step_logic.cpp` — id-based pure nav; drop `WizardSkipFlags` struct-of-bools.
- `include/ui_wizard_*.h` + `src/ui/ui_wizard_*.cpp` (×13) — inherit `Step`, add `id()`/`component_name()`/`log_name()`, port `should_skip(ctx)`.
- `src/ui/wizard_step_registry.h` + `.cpp` — **new**: ordered registry + lookups + `StepContext` builder.
- `src/ui/ui_wizard.cpp` — registry-driven dispatch; delete 13 static bools + central preset `if/else` + all int branches; `current_screen_step` → `StepId`.
- `tests/unit/test_wizard_step_logic.cpp` — id-based nav + per-step skip + regression tests.

---

## Task 1: Step base class, StepId, StepContext

**Files:**
- Create: `include/wizard_step.h`
- Test: `tests/unit/test_wizard_step_logic.cpp` (compile-smoke only here)

- [ ] **Step 1: Create the header**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step_logic.h" // WizardPresetPlan
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
namespace helix { class Config; }
class MoonrakerAPI;

namespace helix::wizard {

enum class StepId {
    TouchCalibration = 0, Language, Wifi, Connection, PrinterIdentify,
    HeaterSelect, FanSelect, AmsIdentify, LedSelect, FilamentSensor,
    InputShaper, Summary, Telemetry,
};
inline constexpr int kStepCount = 13;

// Plain data needed to decide skips — no LVGL, constructible in tests.
struct StepContext {
    helix::Config* config = nullptr;
    MoonrakerAPI* api = nullptr;
    WizardPresetPlan preset{};          // {skip_hardware, first_run}
    bool is_subsequent_printer = false;
    bool is_fbdev = false;
    bool force_language_step = false;
};

class Step {
  public:
    virtual ~Step() = default;
    virtual StepId id() const = 0;
    virtual const char* component_name() const = 0; // e.g. "wizard_heater_select"
    virtual const char* log_name() const = 0;
    virtual bool should_skip(const StepContext&) const { return false; }
    virtual void init_subjects() = 0;
    virtual void register_callbacks() = 0;
    virtual lv_obj_t* create(lv_obj_t* parent) = 0;
    virtual void cleanup() = 0;
    virtual bool is_validated() const { return true; }
};

const char* to_string(StepId); // for logs/debug

} // namespace helix::wizard
```

- [ ] **Step 2: Add `to_string(StepId)`** in a new `src/system/wizard_step.cpp` returning the enumerator name (e.g. `"HeaterSelect"`); add the file to the build if the Makefile globs `src/**` it is automatic — verify with `make test`.

- [ ] **Step 3: Build** — `make test 2>&1 | tail -3`. Expected: links OK (header unused yet is fine; the .cpp must compile).

- [ ] **Step 4: Commit** — `git add include/wizard_step.h src/system/wizard_step.cpp && git commit -m "feat(wizard): add Step base class, StepId enum, StepContext"`

---

## Task 2: Pure id-based navigation (TDD)

Replace the `WizardSkipFlags` struct-of-bools nav with id-based nav over an
ordered `[{StepId, skipped}]`. Caller computes `skipped` via `Step::should_skip`.

**Files:**
- Modify: `include/wizard_step_logic.h`, `src/system/wizard_step_logic.cpp`
- Test: `tests/unit/test_wizard_step_logic.cpp`

- [ ] **Step 1: Write failing tests** (append):

```cpp
#include "wizard_step.h"
using helix::wizard::StepId;

static std::vector<helix::StepSkip> all_visible() {
    std::vector<helix::StepSkip> v;
    for (int i = 0; i < helix::wizard::kStepCount; ++i)
        v.push_back({static_cast<StepId>(i), false});
    return v;
}

TEST_CASE("id-nav: next walks visible steps", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    REQUIRE(helix::wizard_next(StepId::Connection, v) == StepId::PrinterIdentify);
    REQUIRE(helix::wizard_visible_count(v) == 13);
}

TEST_CASE("id-nav: non-contiguous skips are honored", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    // skip HeaterSelect, AmsIdentify, InputShaper (non-adjacent)
    for (auto& s : v) if (s.id == StepId::HeaterSelect || s.id == StepId::AmsIdentify ||
                          s.id == StepId::InputShaper) s.skipped = true;
    REQUIRE(helix::wizard_next(StepId::PrinterIdentify, v) == StepId::FanSelect); // skips Heater
    REQUIRE(helix::wizard_next(StepId::FanSelect, v) == StepId::LedSelect);       // skips Ams
    REQUIRE(helix::wizard_next(StepId::FilamentSensor, v) == StepId::Summary);    // skips InputShaper
    REQUIRE(helix::wizard_visible_count(v) == 10);
}

TEST_CASE("id-nav: last visible step reports done", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    for (auto& s : v) if (s.id == StepId::Telemetry) s.skipped = true;
    REQUIRE(helix::wizard_is_last(StepId::Summary, v));
    REQUIRE_FALSE(helix::wizard_next(StepId::Summary, v).has_value());
}

TEST_CASE("id-nav: display number counts visible predecessors", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    for (auto& s : v) if (s.id == StepId::TouchCalibration || s.id == StepId::Language) s.skipped = true;
    REQUIRE(helix::wizard_display_number(StepId::Wifi, v) == 1);
    REQUIRE(helix::wizard_display_number(StepId::Connection, v) == 2);
}
```

- [ ] **Step 2: Declare API** in `wizard_step_logic.h` (keep the existing int-based fns for now so the build stays green until Task 18 removes them):

```cpp
#include <optional>
#include <vector>
namespace helix {
namespace wizard { enum class StepId; }
struct StepSkip { wizard::StepId id; bool skipped; };
int wizard_visible_count(const std::vector<StepSkip>&);
int wizard_display_number(wizard::StepId current, const std::vector<StepSkip>&); // 1-based
std::optional<wizard::StepId> wizard_next(wizard::StepId, const std::vector<StepSkip>&);
std::optional<wizard::StepId> wizard_prev(wizard::StepId, const std::vector<StepSkip>&);
bool wizard_is_last(wizard::StepId, const std::vector<StepSkip>&);
}
```
(Note: `wizard_next/prev` return `std::optional`; tests use `.has_value()` / `==`.)

- [ ] **Step 3: Run tests, verify FAIL** — `make test && ./build/bin/helix-tests "[idnav]"`. Expected: link/compile fail or assertion fail.

- [ ] **Step 4: Implement** in `wizard_step_logic.cpp`: find current index in the vector; for `next`, scan forward for first `!skipped` → return its id, else `nullopt`; `prev` scans backward; `visible_count` counts `!skipped`; `display_number` = 1 + count of visible before current; `is_last` = `next == nullopt`.

- [ ] **Step 5: Run tests, verify PASS** — `./build/bin/helix-tests "[idnav]"`. Expected: all pass.

- [ ] **Step 6: Commit** — `git commit -am "feat(wizard): id-based pure navigation over step registry"`

---

## Tasks 3–15: Port each step to `helix::wizard::Step`

For EACH of the 13 steps below, do the same mechanical port. Template (worked
example = HeaterSelect) followed by the per-step deltas table.

**Template (per step):**
1. In `include/ui_wizard_<x>.h`: `#include "wizard_step.h"`; change the class to
   `class Wizard<X>Step : public helix::wizard::Step`. Add overrides:
   ```cpp
   helix::wizard::StepId id() const override { return helix::wizard::StepId::<Id>; }
   const char* component_name() const override { return "<xml_component>"; }
   const char* log_name() const override { return "<existing get_name() string>"; }
   bool should_skip(const helix::wizard::StepContext& ctx) const override; // if it skips
   ```
   Mark existing `init_subjects/register_callbacks/create/cleanup` (and
   `is_validated` if present) `override`. Remove the old `get_name()` (rename body
   into `log_name()`). If the old `should_skip()` took no args, change signature.
2. In `src/ui/ui_wizard_<x>.cpp`: implement `should_skip(ctx)` per the table.
   Reuse the existing detection helpers (e.g. AMS-present, leds(), accelerometer)
   but read preset state from `ctx.preset`, not from globals.
3. Build: `make test 2>&1 | tail -3`. Commit: `git commit -am "refactor(wizard): port <X> step to Step base class"`.

**Per-step deltas:**

| Task | Step | StepId | component_name | should_skip(ctx) body |
|---|---|---|---|---|
| 3 | TouchCalibration | `TouchCalibration` | `wizard_touch_calibration` | `return !ctx.is_fbdev \|\| already_calibrated();` (keep existing calibrated check) |
| 4 | Language | `Language` | `wizard_language_chooser` | `return language_already_set() && !ctx.force_language_step;` |
| 5 | Wifi | `Wifi` | `wizard_wifi_setup` | `return helix::is_android_platform() \|\| ctx.is_subsequent_printer;` |
| 6 | Connection | `Connection` | `wizard_connection` | `return ctx.preset.first_run && connection_already_validated();` (keep existing auto-validate check; if none, `return false;`) |
| 7 | PrinterIdentify | `PrinterIdentify` | `wizard_printer_identify` | `return ctx.preset.skip_hardware;` |
| 8 | HeaterSelect | `HeaterSelect` | `wizard_heater_select` | `return ctx.preset.skip_hardware;` |
| 9 | FanSelect | `FanSelect` | `wizard_fan_select` | `return ctx.preset.skip_hardware;` |
| 10 | AmsIdentify | `AmsIdentify` | `wizard_ams_identify` | `return ctx.preset.skip_hardware \|\| no_ams_present();` (existing AmsType::NONE check) |
| 11 | LedSelect | `LedSelect` | `wizard_led_select` | `return ctx.preset.skip_hardware \|\| leds_empty(ctx.api);` (existing leds() check) |
| 12 | FilamentSensor | `FilamentSensor` | `wizard_filament_sensor_select` | `return ctx.preset.skip_hardware \|\| fewer_than_two_standalone();` — see Task 16 for the auto-config side effect |
| 13 | InputShaper | `InputShaper` | `wizard_input_shaper` | `return ctx.preset.skip_hardware ? preset_resonance_done(ctx) : !has_accelerometer();` — see Task 17 for `preset_resonance_done` |
| 14 | Summary | `Summary` | `wizard_summary` | `return ctx.preset.first_run;` |
| 15 | Telemetry | `Telemetry` | `wizard_telemetry` | `return !ctx.preset.first_run;` |

After all 13: `make test` builds; no behavior wired yet (ui_wizard.cpp still uses
old paths). This is intentional — steps now ALSO satisfy the `Step` interface.

---

## Task 16: FilamentSensor auto-config side effect

The single-sensor auto-config currently runs in `ui_wizard.cpp`'s nav cascade when
the filament step is skipped for `<2 standalone sensors`. It must still run in that
case, but NOT when skipped by preset (preset already configured sensors).

**Files:** `include/ui_wizard_filament_sensor_select.h`, `src/ui/ui_wizard_filament_sensor_select.cpp`

- [ ] **Step 1:** Add `void on_skipped_for_sparse_sensors();` that runs the existing
  `auto_configure_single_sensor()` when `get_standalone_sensor_count() == 1`.
- [ ] **Step 2:** Document the contract: the registry dispatch (Task 18) calls this
  exactly when `should_skip` is true due to the sparse-sensor branch (not preset).
  Implement a helper `bool skipped_due_to_sparse_sensors(const StepContext& ctx)`
  = `!ctx.preset.skip_hardware && fewer_than_two_standalone()`.
- [ ] **Step 3:** Build + commit: `git commit -am "refactor(wizard): move filament single-sensor auto-config onto the step"`

---

## Task 17: InputShaper preset-resonance flag

**Files:** `src/ui/ui_wizard_input_shaper.cpp`, spec table.

- [ ] **Step 1:** Add `static bool preset_resonance_done(const StepContext& ctx)`:
  read `initial_resonance_compensation_run` from the active preset/config; **default
  `true`** when absent. (Key: `/printers/<active>/initial_resonance_compensation_run`
  or a preset field — match how other preset fields are read in `config.cpp`.)
- [ ] **Step 2:** `should_skip` already calls it (Task 13). Build + commit:
  `git commit -am "feat(wizard): per-preset initial_resonance_compensation_run flag (default true)"`

---

## Task 18: Registry + StepContext builder

**Files:**
- Create: `src/ui/wizard_step_registry.h`, `src/ui/wizard_step_registry.cpp`

- [ ] **Step 1:** Declare:
```cpp
namespace helix::wizard {
const std::vector<Step*>& steps();              // ordered, lazily built from get_wizard_*_step()
Step* step_by_id(StepId);
StepContext build_context();                    // from Config/api/preset_plan
std::vector<helix::StepSkip> skip_vector(const StepContext&); // {id, step->should_skip(ctx)} in order
}
```
- [ ] **Step 2:** Implement `steps()` to call each `get_wizard_*_step()` in StepId
  order (triggering their existing lazy singleton + StaticPanelRegistry cleanup) and
  cache the ordered `Step*` vector. `step_by_id` indexes by `static_cast<int>(id)`.
- [ ] **Step 3:** `build_context()` fills `StepContext` from `Config::get_instance()`,
  `get_moonraker_api()`, `wizard_preset_plan(cfg->has_preset(), cfg->get_printer_ids().size())`,
  `is_subsequent_printer = cfg->get_printer_ids().size() > 1`, `is_fbdev` (existing
  detection), `force_language_step` (existing global).
- [ ] **Step 4:** Build + commit: `git commit -am "feat(wizard): step registry + StepContext builder"`

---

## Task 19: Collapse ui_wizard.cpp onto the registry (keystone)

**Files:** `src/ui/ui_wizard.cpp`

- [ ] **Step 1:** Change `current_screen_step` to `helix::wizard::StepId`
  (add a `std::optional<StepId> current_screen_step` for the "nothing loaded" state).
- [ ] **Step 2:** Replace the two 13-case switches (`ui_wizard_load_screen`,
  `ui_wizard_cleanup_current_screen`) with: resolve `Step* s = step_by_id(id)` then
  call `s->init_subjects(); s->register_callbacks(); s->create(content);` and
  `s->cleanup();`. Title via `get_step_title_from_xml(s->component_name())`.
- [ ] **Step 3:** Replace `precalculate_skips()` + the 13 static `*_step_skipped`
  bools + the central preset `if/else` with: build `ctx = build_context()` and
  `auto skips = skip_vector(ctx)` at navigation time; use
  `wizard_next/prev/visible_count/display_number/is_last`. Delete the static bools
  and `get_current_skip_flags()`.
- [ ] **Step 4:** Replace `on_next_clicked` / `on_back_clicked` int cascades with
  `wizard_next(current_id, skips)` / `wizard_prev(...)`. On filament skip, call
  `filament step->on_skipped_for_sparse_sensors()` iff
  `skipped_due_to_sparse_sensors(ctx)`.
- [ ] **Step 5:** Convert `ui_wizard_navigate_to_step(int)` to
  `ui_wizard_navigate_to_step(StepId)`; update all callers. Any XML/callback passing
  a step token resolves a `component_name()` string → `StepId` at the single edge.
- [ ] **Step 6:** Build: `make test 2>&1 | tail -3` (expect green). Run wizard tests.
- [ ] **Step 7:** Commit: `git commit -am "refactor(wizard): drive dispatch + skips from the StepId registry (remove 45 int branches)"`

---

## Task 20: Regression + side-effect tests

**Files:** `tests/unit/test_wizard_step_logic.cpp`

- [ ] **Step 1: Tests** (per-step skip via `StepContext`):

```cpp
#include "wizard_step_registry.h"
using helix::wizard::StepId;

static helix::wizard::StepContext ctx_for(bool has_preset, int printers,
                                          bool sub) {
    helix::wizard::StepContext c;
    c.preset = helix::wizard_preset_plan(has_preset, printers);
    c.is_subsequent_printer = sub;
    return c;
}

TEST_CASE("skip: subsequent preset printer skips hardware, shows summary, no telemetry",
          "[wizard][step_logic][regression]") {
    auto ctx = ctx_for(/*has_preset=*/true, /*printers=*/2, /*sub=*/true);
    auto* reg = helix::wizard::steps();
    auto skipped = [&](StepId id){ return helix::wizard::step_by_id(id)->should_skip(ctx); };
    REQUIRE(skipped(StepId::PrinterIdentify));
    REQUIRE(skipped(StepId::HeaterSelect));
    REQUIRE(skipped(StepId::FanSelect));
    REQUIRE(skipped(StepId::AmsIdentify));
    REQUIRE(skipped(StepId::LedSelect));
    REQUIRE(skipped(StepId::InputShaper));
    REQUIRE_FALSE(skipped(StepId::Summary));   // shown for subsequent printer
    REQUIRE(skipped(StepId::Telemetry));       // never re-prompt
}

TEST_CASE("skip: first-run preset printer skips summary, shows telemetry",
          "[wizard][step_logic][regression]") {
    auto ctx = ctx_for(true, 1, false);
    REQUIRE(helix::wizard::step_by_id(StepId::Summary)->should_skip(ctx));
    REQUIRE_FALSE(helix::wizard::step_by_id(StepId::Telemetry)->should_skip(ctx));
}
```

  (If `should_skip` for ams/led/input_shaper depends on live LVGL/api state that a
  unit test can't supply, the test sets `ctx.preset.skip_hardware=true` and asserts
  the preset branch dominates — i.e. skip is true regardless of hardware.)

- [ ] **Step 2:** Run `[regression]` → FAIL if any wiring wrong, then confirm PASS.
- [ ] **Step 3:** Commit: `git commit -am "test(wizard): registry skip + K2-subsequent-preset regression"`

---

## Task 21: Sweep for stray integers + full verification

- [ ] **Step 1:** `grep -nE "step == [0-9]|next_step == [0-9]|case [0-9]+:|STEP_COMPONENT_NAMES|_step_skipped|WizardSkipFlags|get_current_skip_flags|precalculate_skips" src/ui/ui_wizard.cpp include/wizard_step_logic.h` — expect ZERO hits (all removed). Fix any remaining.
- [ ] **Step 2:** Remove now-dead `STEP_COMPONENT_NAMES`, `WizardSkipFlags`, the old int-based `wizard_next_step/prev_step/calculate_*` if fully unused (check tests first; migrate any remaining test to id-based).
- [ ] **Step 3:** `make test-run` (full suite) — expect green (allow pre-existing unrelated `test_print_status_widget_idle_thumb` failure noted earlier).
- [ ] **Step 4:** `./build/bin/helix-tests "[wizard]"` — all pass.
- [ ] **Step 5:** Commit: `git commit -am "refactor(wizard): remove dead int-based step machinery"`

---

## Self-Review notes

- **Spec coverage:** base class (T1), named steps/enum (T1,T3-15), registry (T18),
  per-step skip (T3-15), pure testable nav (T2,T20), filament side-effect (T16),
  resonance factory flag (T17), full StepId / no int seam (T19,T21). ✓
- **Type consistency:** `StepId`, `StepContext`, `StepSkip`, `Step`, `steps()`,
  `step_by_id`, `build_context`, `skip_vector`, `wizard_next/prev/visible_count/
  display_number/is_last` used consistently across tasks. ✓
- **Risk:** Task 19 is the serial keystone; Tasks 3–15 parallelize after T1; T2/T16/
  T17 independent. Run T21 grep last to prove no integer survivors.

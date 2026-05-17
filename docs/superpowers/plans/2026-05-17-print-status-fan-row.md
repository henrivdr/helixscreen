# Print Status Fan Row Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a horizontal 3-fan info row (part/hotend/aux) to the print status panel's right column. The row appears between the AMS/filament status row and the action buttons, hides itself when the column can't fit it, and click-jumps to the existing fan control overlay.

**Architecture:** New XML component embedded in `print_status_panel.xml`. C++ panel observes per-fan speeds via dynamic subjects (paired `SubjectLifetime` per [L084]), reuses `fan_spin_animation`, and drives a new `print_status_fans_fit` int subject that XML binds to `hidden`. The fan classification loop is extracted from `FanStackWidget::bind_fans()` into a `PrinterFanState::classify_primary_fans()` helper so the two callers share one source of truth.

**Tech Stack:** LVGL 9.5 with the helix-xml engine, design tokens (`#primary`, `#space_sm`, etc.), `ObserverGuard` + `SubjectLifetime` RAII pair, `AsyncLifetimeGuard` for deferred work, spdlog for logging, Catch2 (via `XMLTestFixture`) for tests.

**Spec:** `docs/superpowers/specs/2026-05-17-print-status-fan-row-design.md`

---

## File Structure

| File | Status | Responsibility |
|------|--------|----------------|
| `include/printer_fan_state.h` | Modify | Add `PrimaryFans` struct + `classify_primary_fans()` |
| `src/printer/printer_fan_state.cpp` | Modify | Implement `classify_primary_fans()` |
| `src/ui/panel_widgets/fan_stack_widget.cpp` | Modify | Use new helper from `bind_fans()` — no behavior change |
| `ui_xml/components/print_status_fan_row.xml` | Create | The 3-fan horizontal row component |
| `ui_xml/print_status_panel.xml` | Modify | Insert `<print_status_fan_row/>` between filament row and button grid |
| `src/main.cpp` | Modify | Register the new XML component file |
| `include/ui_panel_print_status.h` | Modify | Subject fields, observers + paired lifetimes, fan_control_panel_ ptr, helper decls |
| `src/ui/ui_panel_print_status.cpp` | Modify | Register subjects, wire observers, `recompute_fans_fit()`, click handler, lazy overlay create |
| `tests/unit/test_print_status_fan_section.cpp` | Create | Unit tests for fan section behavior + drift test for the helper extraction |

---

## Task 1: Extract `classify_primary_fans()` Helper (TDD)

**Files:**
- Modify: `include/printer_fan_state.h`
- Modify: `src/printer/printer_fan_state.cpp`
- Test: `tests/unit/test_print_status_fan_section.cpp` (new file, just this test first)

- [ ] **Step 1: Write the failing drift test**

Create `tests/unit/test_print_status_fan_section.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_fan_state.h"
#include "tests/helix_test_fixture.h"

#include <catch2/catch_test_macros.hpp>

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first of each type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    // Synthesize three fans of different types (using the public update path)
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    fans.push_back({"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 80, false,
                    std::nullopt});
    fans.push_back({"fan_generic chamber", "Chamber", FanType::GENERIC_FAN, 0, true, std::nullopt});
    state.set_fans_for_test(fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend == "heater_fan hotend_fan");
    REQUIRE(picked.aux == "fan_generic chamber");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans returns empty when no fans of type",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt});
    state.set_fans_for_test(fans);

    auto picked = state.classify_primary_fans();
    REQUIRE(picked.part == "fan");
    REQUIRE(picked.hotend.empty());
    REQUIRE(picked.aux.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans picks first not extras",
                 "[fan_state][drift]") {
    PrinterFanState state;
    std::vector<FanInfo> fans;
    fans.push_back({"fan", "Part 1", FanType::PART_COOLING, 0, true, std::nullopt});
    fans.push_back({"fan_generic part2", "Part 2", FanType::PART_COOLING, 0, true, std::nullopt});
    state.set_fans_for_test(fans);

    REQUIRE(state.classify_primary_fans().part == "fan");
}

TEST_CASE_METHOD(HelixTestFixture, "classify_primary_fans treats controller/temp/generic/output as aux",
                 "[fan_state][drift]") {
    for (FanType aux_type : {FanType::CONTROLLER_FAN, FanType::TEMPERATURE_FAN,
                              FanType::GENERIC_FAN, FanType::OUTPUT_PIN_FAN}) {
        PrinterFanState state;
        std::vector<FanInfo> fans;
        fans.push_back({"fan_x", "X", aux_type, 0, false, std::nullopt});
        state.set_fans_for_test(fans);
        REQUIRE_FALSE(state.classify_primary_fans().aux.empty());
    }
}
```

If `set_fans_for_test` doesn't exist yet, this test also drives the need for a test-only setter (next step).

- [ ] **Step 2: Run test, confirm compilation failure**

Run: `make test 2>&1 | tail -20`
Expected: compile error — `classify_primary_fans` not declared, `set_fans_for_test` not declared.

- [ ] **Step 3: Add struct + method declaration to header**

Edit `include/printer_fan_state.h`. After the `FanInfo` struct (around line 63) but before `class PrinterFanState`, add:

```cpp
/**
 * @brief Picked primary fan object names for the standard 3-fan display.
 *
 * Each member is the object_name of the first fan of that role found in
 * the discovered fan list (in fan-discovery order), or empty if none.
 */
struct PrimaryFans {
    std::string part;   ///< First PART_COOLING fan, or empty
    std::string hotend; ///< First HEATER_FAN, or empty
    std::string aux;    ///< First of CONTROLLER_FAN, TEMPERATURE_FAN,
                        ///< GENERIC_FAN, OUTPUT_PIN_FAN — or empty
};
```

In `class PrinterFanState`'s public section, near `classify_fan_type()` (line 158), add:

```cpp
    /// Pick first fan of each primary role from the discovered list.
    /// Returns empty strings for roles with no detected fan.
    PrimaryFans classify_primary_fans() const;
```

And in the public section, after the existing accessors, add a test-only setter:

```cpp
#ifdef HELIX_TEST_BUILD
    /// Test-only: inject a synthesized fan list, bypassing Moonraker discovery.
    void set_fans_for_test(std::vector<FanInfo> fans) { fans_ = std::move(fans); }
#endif
```

- [ ] **Step 4: Implement the helper**

Edit `src/printer/printer_fan_state.cpp`. Add after `classify_fan_type()` (search for `FanType PrinterFanState::classify_fan_type`):

```cpp
PrimaryFans PrinterFanState::classify_primary_fans() const {
    PrimaryFans out;
    for (const auto& fan : fans_) {
        switch (fan.type) {
        case FanType::PART_COOLING:
            if (out.part.empty())
                out.part = fan.object_name;
            break;
        case FanType::HEATER_FAN:
            if (out.hotend.empty())
                out.hotend = fan.object_name;
            break;
        case FanType::CONTROLLER_FAN:
        case FanType::TEMPERATURE_FAN:
        case FanType::GENERIC_FAN:
        case FanType::OUTPUT_PIN_FAN:
            if (out.aux.empty())
                out.aux = fan.object_name;
            break;
        }
    }
    return out;
}
```

- [ ] **Step 5: Add `HELIX_TEST_BUILD` to the test build define list**

Check `mk/tests.mk` for the existing test defines. If `HELIX_TEST_BUILD` isn't already set, add:

```make
TESTS_CXXFLAGS += -DHELIX_TEST_BUILD
```

If a similar test-only macro already exists (e.g. `HELIX_TESTING`), use that instead and update the `#ifdef` in the header to match.

- [ ] **Step 6: Run the tests, confirm pass**

Run: `make test-run TEST_ARGS="[fan_state][drift]" 2>&1 | tail -30`
Expected: all 4 cases pass.

- [ ] **Step 7: Commit**

```bash
git add include/printer_fan_state.h src/printer/printer_fan_state.cpp tests/unit/test_print_status_fan_section.cpp mk/tests.mk
git commit -m "feat(fan_state): extract classify_primary_fans helper"
```

---

## Task 2: Refactor `FanStackWidget::bind_fans()` to use the helper

**Files:**
- Modify: `src/ui/panel_widgets/fan_stack_widget.cpp:337-415`

- [ ] **Step 1: Replace the in-line classification loop**

In `bind_fans()`, replace lines 357-385 (the `// Classify fans into our three rows` block through the closing brace of the `for` loop) with:

```cpp
    auto primary = printer_state_.get_fan_state().classify_primary_fans();
    part_fan_name_ = primary.part;
    hotend_fan_name_ = primary.hotend;
    aux_fan_name_ = primary.aux;

    // Resolve display names from the discovered fan list (helper picks by
    // object_name, but the widget still wants the human-readable label).
    part_display_name_.clear();
    hotend_display_name_.clear();
    aux_display_name_.clear();
    for (const auto& fan : fans) {
        if (fan.object_name == part_fan_name_)
            part_display_name_ = fan.display_name;
        else if (fan.object_name == hotend_fan_name_)
            hotend_display_name_ = fan.display_name;
        else if (fan.object_name == aux_fan_name_)
            aux_display_name_ = fan.display_name;
    }
```

If `printer_state_.get_fan_state()` doesn't exist as a public accessor (check `include/printer_state.h`), add one returning `const PrinterFanState&`. Look for existing accessors like `get_temp_sensor_manager()` for the pattern.

- [ ] **Step 2: Run the existing fan widget tests, confirm no regression**

Run: `make test-run TEST_ARGS="[fan]" 2>&1 | tail -30`
Expected: all pre-existing fan tests pass (same behavior, different code path).

- [ ] **Step 3: Commit**

```bash
git add src/ui/panel_widgets/fan_stack_widget.cpp include/printer_state.h
git commit -m "refactor(fan_stack): use classify_primary_fans helper"
```

---

## Task 3: Create `print_status_fan_row.xml` Component

**Files:**
- Create: `ui_xml/components/print_status_fan_row.xml`

- [ ] **Step 1: Write the XML component**

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Horizontal 3-fan info row for the print status panel. -->
<!-- Visibility is driven by print_status_fans_fit (adaptive fit calc). -->
<!-- Aux cluster is driven by print_status_aux_fan_present. -->
<component>
  <view name="print_status_fan_row_root"
        extends="lv_obj"
        width="100%" height="content"
        style_pad_all="0" style_pad_left="#space_md" style_pad_right="#space_md"
        flex_flow="row"
        style_flex_main_place="center" style_flex_cross_place="center"
        style_pad_gap="#space_sm"
        scrollable="false" clickable="true">

    <!-- Hide whole row when adaptive-fit calc says no -->
    <bind_flag_if_eq subject="print_status_fans_fit" flag="hidden" ref_value="0"/>

    <event_cb trigger="clicked" callback="on_print_status_fans_clicked"/>

    <!-- Part fan -->
    <icon name="part_fan_icon" src="fan" size="xs" variant="secondary"
          clickable="false" event_bubble="true"/>
    <text_small name="part_fan_label" text="Part" translation_tag="Part"
                style_text_color="#primary"
                clickable="false" event_bubble="true"/>
    <text_small name="part_fan_speed" text="--"
                clickable="false" event_bubble="true"/>

    <!-- Separator before hotend -->
    <text_small text=" · " style_text_color="#text_muted"
                clickable="false" event_bubble="true"/>

    <!-- Hotend fan -->
    <icon name="hotend_fan_icon" src="fan" size="xs" variant="secondary"
          clickable="false" event_bubble="true"/>
    <text_small name="hotend_fan_label" text="Hotend" translation_tag="Hotend"
                style_text_color="#primary"
                clickable="false" event_bubble="true"/>
    <text_small name="hotend_fan_speed" text="--"
                clickable="false" event_bubble="true"/>

    <!-- Aux cluster: separator + icon + label + speed, all hide together -->
    <text_small name="aux_separator" text=" · " style_text_color="#text_muted"
                clickable="false" event_bubble="true">
      <bind_flag_if_eq subject="print_status_aux_fan_present" flag="hidden" ref_value="0"/>
    </text_small>
    <icon name="aux_fan_icon" src="fan" size="xs" variant="secondary"
          clickable="false" event_bubble="true">
      <bind_flag_if_eq subject="print_status_aux_fan_present" flag="hidden" ref_value="0"/>
    </icon>
    <text_small name="aux_fan_label" text="Aux" translation_tag="Aux"
                style_text_color="#primary"
                clickable="false" event_bubble="true">
      <bind_flag_if_eq subject="print_status_aux_fan_present" flag="hidden" ref_value="0"/>
    </text_small>
    <text_small name="aux_fan_speed" text="--"
                clickable="false" event_bubble="true">
      <bind_flag_if_eq subject="print_status_aux_fan_present" flag="hidden" ref_value="0"/>
    </text_small>
  </view>
</component>
```

Note: the `·` is the middle-dot Unicode character. If the XML parser doesn't accept `\u` escapes, replace with the literal UTF-8 bytes (`·`) or with `&#xB7;`. Match the style used in existing components — grep `ui_xml/` for `·` to find precedent.

- [ ] **Step 2: Verify with grep that the middot is already used in XML**

Run: `grep -rn '·' ui_xml/components/ | head -5`

If found: use that literal character. If not found: use `&#xB7;` entity. Update the XML accordingly before commit.

- [ ] **Step 3: Commit** (registration and integration in next tasks; this commit is just the standalone component)

```bash
git add ui_xml/components/print_status_fan_row.xml
git commit -m "feat(ui): print_status_fan_row XML component"
```

---

## Task 4: Register XML Component in main.cpp

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Find where similar components are registered**

Run: `grep -n "lv_xml_component_register_from_file" src/main.cpp | head -20`
Pick a recent similar component registration to mirror the style (path + error handling).

- [ ] **Step 2: Add the registration**

Insert the registration alongside the other `components/` registrations, in alphabetical order. Pattern (substitute the surrounding style you find):

```cpp
    if (lv_xml_component_register_from_file(
            "ui_xml/components/print_status_fan_row.xml") != LV_RESULT_OK) {
        spdlog::error("[main] Failed to register print_status_fan_row component");
    }
```

Per [L014]: forgetting this registration = silent runtime failure (component not found).

- [ ] **Step 3: Build to confirm it compiles**

Run: `make -j 2>&1 | tail -10`
Expected: clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(ui): register print_status_fan_row component"
```

---

## Task 5: Add Subjects to PrintStatusPanel

**Files:**
- Modify: `include/ui_panel_print_status.h`
- Modify: `src/ui/ui_panel_print_status.cpp`

- [ ] **Step 1: Add subject fields to the header**

Edit `include/ui_panel_print_status.h`. In the `private:` section, alongside the other `lv_subject_t` members (search for `_subject_;` to find the block — there's a cluster of them), add:

```cpp
    // Fan row visibility (adaptive: hidden when the column can't fit it)
    lv_subject_t fans_fit_subject_{};
    // Aux fan present (hides the aux cluster when 0)
    lv_subject_t aux_fan_present_subject_{};

    // Cached natural height of the fan row (measured at attach, while
    // forced-visible). Used by recompute_fans_fit() as `needed`.
    int fan_row_natural_height_ = 0;

    // Resolved fan object names (refreshed when fans_version ticks)
    std::string part_fan_name_;
    std::string hotend_fan_name_;
    std::string aux_fan_name_;
```

In the same `private:` section, alongside the existing observer guards (search for `ObserverGuard ` to find the cluster), add:

```cpp
    // Per-fan speed observers — each on a DYNAMIC subject, so paired
    // SubjectLifetime members are mandatory per [L084].
    ObserverGuard part_speed_observer_;
    SubjectLifetime part_speed_lifetime_;
    ObserverGuard hotend_speed_observer_;
    SubjectLifetime hotend_speed_lifetime_;
    ObserverGuard aux_speed_observer_;
    SubjectLifetime aux_speed_lifetime_;

    // Static-subject observers (no lifetime token needed)
    ObserverGuard fans_version_observer_;
    ObserverGuard animations_enabled_observer_;
    ObserverGuard breakpoint_observer_;
    ObserverGuard filament_sensor_count_observer_;
    ObserverGuard ams_slot_count_observer_;
    ObserverGuard toolchange_visible_observer_;

    // Lazy fan control overlay (created on first click)
    lv_obj_t* fan_control_panel_ = nullptr;
```

Make sure these go BEFORE the `helix::AsyncLifetimeGuard lifetime_;` line — per the existing pattern (already declared LAST so it's torn down first, expiring captured tokens before observers destruct).

Also include the necessary header in the `.h`:

```cpp
#include "ui_observer_guard.h"  // ObserverGuard, SubjectLifetime — likely already included
```

- [ ] **Step 2: Register the new subjects in `init_subjects()`**

Edit `src/ui/ui_panel_print_status.cpp`. In `init_subjects()`, just after the cluster of `UI_MANAGED_SUBJECT_INT` calls for `gcode_viewer_mode`/`exclude_map_active`/`end_overlay_dismissed` (around line 407-409), add:

```cpp
    // Fan row adaptive-fit + aux presence subjects (set by recompute_fans_fit
    // and bind_fan_speeds respectively; default to 0 so the row stays hidden
    // until the first recompute fires after attach).
    UI_MANAGED_SUBJECT_INT(fans_fit_subject_, 0, "print_status_fans_fit", subjects_);
    UI_MANAGED_SUBJECT_INT(aux_fan_present_subject_, 0, "print_status_aux_fan_present",
                           subjects_);
```

- [ ] **Step 3: Add the click callback registration**

In the `register_xml_callbacks({...})` block (around line 452-461), add a new entry:

```cpp
        {"on_print_status_fans_clicked", on_fans_clicked},
```

- [ ] **Step 4: Declare the static dispatch in the header**

In `include/ui_panel_print_status.h`'s `private:` section, alongside the other `on_*_clicked` static methods (search for `on_tune_clicked`), add:

```cpp
    static void on_fans_clicked(lv_event_t* e);
    void handle_fans_click();
```

- [ ] **Step 5: Stub the static dispatch in the cpp**

In `src/ui/ui_panel_print_status.cpp`, alongside the other `on_*_clicked` definitions (search for `void PrintStatusPanel::on_tune_clicked`), add:

```cpp
void PrintStatusPanel::on_fans_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_fans_clicked");
    get_global_print_status_panel().handle_fans_click();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusPanel::handle_fans_click() {
    // Stub — full overlay-push wired in Task 9
    spdlog::debug("[PrintStatusPanel] fans clicked (stub)");
}
```

- [ ] **Step 6: Build, confirm no errors**

Run: `make -j 2>&1 | tail -10`
Expected: clean build.

- [ ] **Step 7: Commit**

```bash
git add include/ui_panel_print_status.h src/ui/ui_panel_print_status.cpp
git commit -m "feat(print_status): add fan row subjects + click stub"
```

---

## Task 6: Embed Component in `print_status_panel.xml`

**Files:**
- Modify: `ui_xml/print_status_panel.xml`

- [ ] **Step 1: Insert the component between filament_ams_status_row and button_grid**

Edit `ui_xml/print_status_panel.xml`. Find the closing tag of `filament_ams_status_row` (line 331, `</lv_obj>` directly after the `ams_status_row` closing `</lv_obj>`). After that closing tag and before the `<!-- Control Buttons -->` comment (line 333), insert:

```xml
        <!-- Fan info row: part / hotend / aux speeds, click → fan control overlay. -->
        <!-- Visibility driven by print_status_fans_fit subject (computed in C++). -->
        <print_status_fan_row/>
```

- [ ] **Step 2: XML hot-reload smoke test**

Run (foreground first, single instance — per [L060]):

```bash
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv -p print_status 2>&1 | tee /tmp/fan_row_test.log
```

Tell the user: "Launch the print status panel and let me know if the panel itself renders without errors. The fan row should currently be HIDDEN (the fit subject still defaults to 0 — that's expected at this stage)."

Wait for confirmation.

- [ ] **Step 3: Inspect the log for component-registration errors**

Run: `grep -iE "fan_row|register.*component" /tmp/fan_row_test.log | head -20`
Expected: no errors. Component should load without warnings.

- [ ] **Step 4: Commit**

```bash
git add ui_xml/print_status_panel.xml
git commit -m "feat(print_status): insert fan row between filament row and buttons"
```

---

## Task 7: Wire Fan Speed Observers & Spin Animation

**Files:**
- Modify: `include/ui_panel_print_status.h`
- Modify: `src/ui/ui_panel_print_status.cpp`

- [ ] **Step 1: Add helper method declarations to the header**

In `include/ui_panel_print_status.h`'s `private:` section:

```cpp
    void bind_fan_observers();         ///< Reclassify + rebind on fans_version
    void rebind_single_fan(ObserverGuard& guard, SubjectLifetime& lt,
                           const std::string& object_name,
                           const char* speed_label_widget_name,
                           const char* icon_widget_name);
    void update_fan_speed_display(const char* label_name, const char* icon_name, int speed);
    void refresh_fan_animations();
```

Cache the bool:

```cpp
    bool animations_enabled_ = false;
```

- [ ] **Step 2: Add `bind_fan_observers()` to the cpp**

Insert after the `recompute_paused_overlay_visibility()` family of methods (search for that name to find the area). Place near related printer-state observer helpers:

```cpp
void PrintStatusPanel::bind_fan_observers() {
    // Reset existing observers BEFORE the SubjectLifetime they depend on
    // is reset, per the ordering rule. Then drop the lifetimes.
    // Wait — actual rule: SubjectLifetime must be reset BEFORE the
    // ObserverGuard. Do that for each pair.
    part_speed_lifetime_.reset();
    part_speed_observer_.reset();
    hotend_speed_lifetime_.reset();
    hotend_speed_observer_.reset();
    aux_speed_lifetime_.reset();
    aux_speed_observer_.reset();

    auto primary = printer_state_.get_fan_state().classify_primary_fans();
    part_fan_name_ = primary.part;
    hotend_fan_name_ = primary.hotend;
    aux_fan_name_ = primary.aux;

    rebind_single_fan(part_speed_observer_, part_speed_lifetime_,
                      part_fan_name_, "part_fan_speed", "part_fan_icon");
    rebind_single_fan(hotend_speed_observer_, hotend_speed_lifetime_,
                      hotend_fan_name_, "hotend_fan_speed", "hotend_fan_icon");
    rebind_single_fan(aux_speed_observer_, aux_speed_lifetime_,
                      aux_fan_name_, "aux_fan_speed", "aux_fan_icon");

    // Aux cluster visibility — single subject drives 4 widgets via XML
    lv_subject_set_int(&aux_fan_present_subject_, aux_fan_name_.empty() ? 0 : 1);

    spdlog::debug("[{}] Bound fans: part='{}' hotend='{}' aux='{}'", get_name(),
                  part_fan_name_, hotend_fan_name_, aux_fan_name_);
}

void PrintStatusPanel::rebind_single_fan(ObserverGuard& guard, SubjectLifetime& lt,
                                          const std::string& object_name,
                                          const char* speed_label_widget_name,
                                          const char* icon_widget_name) {
    if (object_name.empty()) {
        update_fan_speed_display(speed_label_widget_name, icon_widget_name, 0);
        return;
    }
    lv_subject_t* subj = printer_state_.get_fan_speed_subject(object_name, lt);
    if (!subj) {
        spdlog::warn("[{}] Fan '{}' subject not available", get_name(), object_name);
        return;
    }

    auto token = lifetime_.token();
    std::string label_copy = speed_label_widget_name;
    std::string icon_copy = icon_widget_name;
    guard = helix::ui::observe_int_sync<PrintStatusPanel>(
        subj, this,
        [token, label_copy, icon_copy](PrintStatusPanel* self, int speed) {
            if (token.expired())
                return;
            self->update_fan_speed_display(label_copy.c_str(), icon_copy.c_str(), speed);
        },
        lt);

    // Seed initial value (observer fires only on change)
    update_fan_speed_display(speed_label_widget_name, icon_widget_name,
                              lv_subject_get_int(subj));
}

void PrintStatusPanel::update_fan_speed_display(const char* label_name,
                                                  const char* icon_name, int speed) {
    if (!overlay_root_)
        return;
    lv_obj_t* label = lv_obj_find_by_name(overlay_root_, label_name);
    if (label) {
        char buf[8];
        helix::format::format_percent(speed, buf, sizeof(buf));
        lv_label_set_text(label, buf);
    }
    lv_obj_t* icon = lv_obj_find_by_name(overlay_root_, icon_name);
    if (icon) {
        if (!animations_enabled_ || speed <= 0)
            helix::ui::fan_spin_stop(icon);
        else
            helix::ui::fan_spin_start(icon, speed);
    }
}

void PrintStatusPanel::refresh_fan_animations() {
    if (!overlay_root_)
        return;
    // Read current speeds from subjects and re-apply animation state.
    auto refresh_one = [this](const std::string& name, const char* icon_widget) {
        if (name.empty()) return;
        SubjectLifetime tmp;
        lv_subject_t* s = printer_state_.get_fan_speed_subject(name, tmp);
        if (!s) return;
        lv_obj_t* icon = lv_obj_find_by_name(overlay_root_, icon_widget);
        if (!icon) return;
        int sp = lv_subject_get_int(s);
        if (!animations_enabled_ || sp <= 0)
            helix::ui::fan_spin_stop(icon);
        else
            helix::ui::fan_spin_start(icon, sp);
    };
    refresh_one(part_fan_name_, "part_fan_icon");
    refresh_one(hotend_fan_name_, "hotend_fan_icon");
    refresh_one(aux_fan_name_, "aux_fan_icon");
}
```

- [ ] **Step 3: Wire the version + animation-settings observers**

In `init_subjects()`, just after the new subject registrations from Task 5 step 2, add:

```cpp
    // Fan classification refresh on discovery
    {
        auto token = lifetime_.token();
        fans_version_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
            printer_state_.get_fans_version_subject(), this,
            [token](PrintStatusPanel* self, int /*v*/) {
                if (token.expired()) return;
                self->bind_fan_observers();
                self->recompute_fans_fit();  // aux row visibility may change height
            });
    }

    // Animation-settings refresh
    animations_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
    {
        auto token = lifetime_.token();
        animations_enabled_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
            DisplaySettingsManager::instance().subject_animations_enabled(), this,
            [token](PrintStatusPanel* self, int enabled) {
                if (token.expired()) return;
                self->animations_enabled_ = (enabled != 0);
                self->refresh_fan_animations();
            });
    }
```

Add the includes at the top of the cpp if not already present:

```cpp
#include "display_settings_manager.h"
#include "format_utils.h"
#include "ui/fan_spin_animation.h"
```

- [ ] **Step 4: Bind once after panel creation**

In `create()` (search for `lv_obj_t* PrintStatusPanel::create`), after `overlay_root_` is created and the panel has been laid out for the first time, call `bind_fan_observers()`. A good spot is the section that does initial display-state sync. Pattern:

```cpp
    // Initial fan classification (may rebind later when fans_version updates)
    bind_fan_observers();
```

Place it immediately before the existing `return overlay_root_;` at the end of `create()` (search for `return overlay_root_;` to find it).

- [ ] **Step 5: Reset observers in `deinit_subjects()`**

In `deinit_subjects()` (around line 503), before `subjects_.deinit_all();` is called, add the resets in the correct order (lifetimes before observers per [L084]):

```cpp
    // Fan-row observers — lifetimes BEFORE observer guards per [L084]
    fans_version_observer_.reset();
    animations_enabled_observer_.reset();
    part_speed_lifetime_.reset();
    part_speed_observer_.reset();
    hotend_speed_lifetime_.reset();
    hotend_speed_observer_.reset();
    aux_speed_lifetime_.reset();
    aux_speed_observer_.reset();
```

- [ ] **Step 6: Build**

Run: `make -j 2>&1 | tail -10`
Expected: clean build.

- [ ] **Step 7: Interactive test of fan binding (per [L060])**

Force the `fans_fit_subject_` temporarily to `1` by editing `init_subjects()` (just for this manual test):

```cpp
    UI_MANAGED_SUBJECT_INT(fans_fit_subject_, 1, "print_status_fans_fit", subjects_);  // FORCE 1 for manual test
```

Then run in background:

```bash
./build/bin/helix-screen --test -vv -p print_status 2>&1 | tee /tmp/fan_row_test.log
```

Tell the user: "Look at the print status panel. You should see three fan readouts (Part / Hotend / Aux) with percentages. Mock state typically has the part fan at some value. The aux cluster should only appear if mock state has an aux fan."

Wait for confirmation.

Run: `Read /tmp/fan_row_test.log` (filter for `[PrintStatusPanel] Bound fans:` line).

- [ ] **Step 8: Revert the forced default back to 0**

```cpp
    UI_MANAGED_SUBJECT_INT(fans_fit_subject_, 0, "print_status_fans_fit", subjects_);
```

- [ ] **Step 9: Commit**

```bash
git add include/ui_panel_print_status.h src/ui/ui_panel_print_status.cpp
git commit -m "feat(print_status): bind part/hotend/aux fan observers + spin animation"
```

---

## Task 8: Implement `recompute_fans_fit()` & Triggers

**Files:**
- Modify: `include/ui_panel_print_status.h`
- Modify: `src/ui/ui_panel_print_status.cpp`

- [ ] **Step 1: Declare the method in the header**

In `include/ui_panel_print_status.h`'s `private:` section:

```cpp
    void recompute_fans_fit();
```

- [ ] **Step 2: Add the implementation**

In `src/ui/ui_panel_print_status.cpp`, near `recompute_paused_overlay_visibility()`:

```cpp
void PrintStatusPanel::recompute_fans_fit() {
    if (!overlay_root_)
        return;
    lv_obj_t* controls = lv_obj_find_by_name(overlay_root_, "controls_section");
    lv_obj_t* fan_row = lv_obj_find_by_name(overlay_root_, "print_status_fan_row_root");
    if (!controls || !fan_row)
        return;

    lv_obj_update_layout(controls);

    // Cache fan row natural height the first time we see it visible.
    if (fan_row_natural_height_ == 0) {
        // Temporarily ensure it's measurable: if hidden, the layout still
        // reports content height, so we just use lv_obj_get_height.
        bool was_hidden = lv_obj_has_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        if (was_hidden)
            lv_obj_remove_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(fan_row);
        fan_row_natural_height_ = lv_obj_get_height(fan_row);
        if (was_hidden)
            lv_obj_add_flag(fan_row, LV_OBJ_FLAG_HIDDEN);
        if (fan_row_natural_height_ <= 0) {
            // Layout not settled yet — try again next tick
            auto token = lifetime_.token();
            token.defer("PrintStatusPanel::recompute_fans_fit_retry",
                        [this]() { recompute_fans_fit(); });
            return;
        }
    }

    int controls_h = lv_obj_get_height(controls);
    int used = 0;
    int visible_count = 0;

    auto add_child_height = [&](const char* name) {
        lv_obj_t* o = lv_obj_find_by_name(overlay_root_, name);
        if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return;
        used += lv_obj_get_height(o);
        ++visible_count;
    };

    add_child_height("temp_card");
    add_child_height("speed_flow_row");
    add_child_height("filament_ams_status_row");

    // button_grid: use content height, not the flex_grow=1 stretched height
    lv_obj_t* btn_grid = lv_obj_find_by_name(overlay_root_, "button_grid");
    if (btn_grid && !lv_obj_has_flag(btn_grid, LV_OBJ_FLAG_HIDDEN)) {
        used += lv_obj_get_content_height(btn_grid);
        ++visible_count;
    }

    add_child_height("print_status_extras");

    // Column gaps: (visible_children_total - 1) * space_md
    // We pretend the fan row contributes one visible child to the count if
    // it would be visible — so the gap math is consistent regardless of the
    // current decision (loop safety).
    int gap_count = visible_count;  // gaps between this many siblings + the fan row
    const char* space_md_str = lv_xml_get_const(nullptr, "space_md");
    int space_md = space_md_str ? std::atoi(space_md_str) : 8;
    used += gap_count * space_md;

    int available = controls_h - used;
    int current = lv_subject_get_int(&fans_fit_subject_);
    int next = current;
    if (current == 1) {
        if (available < fan_row_natural_height_)
            next = 0;
    } else {
        if (available >= fan_row_natural_height_ + 4)
            next = 1;
    }

    if (next != current) {
        spdlog::debug(
            "[{}] fans_fit {} -> {} (controls_h={}, used={}, available={}, needed={})",
            get_name(), current, next, controls_h, used, available, fan_row_natural_height_);
        lv_subject_set_int(&fans_fit_subject_, next);
    }
}
```

- [ ] **Step 3: Wire the observer triggers**

In `init_subjects()`, after the existing subject registrations and after `bind_fan_observers()` setup, add observers for the trigger subjects. These are STATIC subjects (no paired lifetime needed):

```cpp
    auto schedule_recompute = [this]() {
        auto token = lifetime_.token();
        token.defer("PrintStatusPanel::recompute_fans_fit",
                    [this]() { recompute_fans_fit(); });
    };

    // ui_breakpoint
    {
        lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
        if (bp) {
            auto token = lifetime_.token();
            breakpoint_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
                bp, this,
                [token, schedule_recompute](PrintStatusPanel* /*self*/, int) {
                    if (token.expired()) return;
                    schedule_recompute();
                });
        }
    }
    // filament_sensor_count
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "filament_sensor_count");
        if (s) {
            auto token = lifetime_.token();
            filament_sensor_count_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
                s, this,
                [token, schedule_recompute](PrintStatusPanel* /*self*/, int) {
                    if (token.expired()) return;
                    schedule_recompute();
                });
        }
    }
    // ams_slot_count
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "ams_slot_count");
        if (s) {
            auto token = lifetime_.token();
            ams_slot_count_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
                s, this,
                [token, schedule_recompute](PrintStatusPanel* /*self*/, int) {
                    if (token.expired()) return;
                    schedule_recompute();
                });
        }
    }
    // toolchange_visible
    {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "toolchange_visible");
        if (s) {
            auto token = lifetime_.token();
            toolchange_visible_observer_ = helix::ui::observe_int_sync<PrintStatusPanel>(
                s, this,
                [token, schedule_recompute](PrintStatusPanel* /*self*/, int) {
                    if (token.expired()) return;
                    schedule_recompute();
                });
        }
    }
```

- [ ] **Step 4: Wire size-changed event on controls_section**

The "no lv_obj_add_event_cb" rule from CLAUDE.md applies to events that have XML bindings (click, value change). `LV_EVENT_SIZE_CHANGED` has no XML binding — direct registration is the established pattern (`src/ui/ui_buffer_meter.cpp:52`, `src/ui/ui_ams_mini_status.cpp:540`, `src/ui/ui_gradient_canvas.cpp:294`).

In `create()`, after `controls_section` is located:

```cpp
    lv_obj_t* controls = lv_obj_find_by_name(overlay_root_, "controls_section");
    if (controls) {
        lv_obj_add_event_cb(controls, on_controls_size_changed,
                            LV_EVENT_SIZE_CHANGED, this);
    }
```

Declare the static dispatch in the header (alongside other `on_*` static methods):

```cpp
    static void on_controls_size_changed(lv_event_t* e);
```

Implement in the cpp:

```cpp
void PrintStatusPanel::on_controls_size_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusPanel] on_controls_size_changed");
    auto* self = static_cast<PrintStatusPanel*>(lv_event_get_user_data(e));
    if (!self) {
        LVGL_SAFE_EVENT_CB_END();
        return;
    }
    auto token = self->lifetime_.token();
    token.defer("PrintStatusPanel::size_changed_recompute",
                [self]() { self->recompute_fans_fit(); });
    LVGL_SAFE_EVENT_CB_END();
}
```

This is safe because `controls` is a child of `overlay_root_` — when the panel tears down, `controls` is deleted with it, so the size-changed callback can never fire after `this` is destroyed. The `tok.defer` adds extra safety against re-entry through the UpdateQueue.

- [ ] **Step 5: Initial recompute after attach**

In `create()`, after `bind_fan_observers()` is called (added in Task 7), and after the panel is fully laid out, add:

```cpp
    {
        auto token = lifetime_.token();
        token.defer("PrintStatusPanel::initial_fans_fit_recompute",
                    [this]() { recompute_fans_fit(); });
    }
```

- [ ] **Step 6: Reset the new observers in `deinit_subjects()`**

Just below the resets added in Task 7 Step 5:

```cpp
    breakpoint_observer_.reset();
    filament_sensor_count_observer_.reset();
    ams_slot_count_observer_.reset();
    toolchange_visible_observer_.reset();
```

- [ ] **Step 7: Build**

Run: `make -j 2>&1 | tail -10`
Expected: clean build.

- [ ] **Step 8: Interactive test (per [L060])**

```bash
./build/bin/helix-screen --test -vv -p print_status 2>&1 | tee /tmp/fan_fit_test.log
```

Tell the user: "Open the print status panel. The fan row should now appear because the column has room. Resize the window smaller — at some point the fan row should disappear; resize back larger and it should reappear."

Wait for confirmation.

- [ ] **Step 9: Inspect log for the fit transitions**

Run: `grep "fans_fit" /tmp/fan_fit_test.log | head -20`
Expected: each transition logs the new value + computed dimensions.

- [ ] **Step 10: Commit**

```bash
git add include/ui_panel_print_status.h src/ui/ui_panel_print_status.cpp
git commit -m "feat(print_status): adaptive fans_fit visibility calculation"
```

---

## Task 9: Wire Click Handler to Fan Control Overlay

**Files:**
- Modify: `src/ui/ui_panel_print_status.cpp`

- [ ] **Step 1: Replace the stub `handle_fans_click()` from Task 5**

Find `handle_fans_click()` (added as a stub in Task 5 step 5). Replace its body with the full overlay-push pattern, mirroring `FanStackWidget::handle_clicked()`:

```cpp
void PrintStatusPanel::handle_fans_click() {
    spdlog::debug("[{}] Fans clicked — opening fan control overlay", get_name());

    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();
        if (!overlay.are_subjects_initialized())
            overlay.init_subjects();
        overlay.register_callbacks();
        overlay.set_api(api_);

        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            spdlog::error("[{}] Failed to create fan control overlay", get_name());
            return;
        }
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        get_fan_control_overlay().set_api(api_);
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}
```

Add includes at the top of the cpp if not already there:

```cpp
#include "ui_fan_control_overlay.h"
#include "ui_nav_manager.h"
```

- [ ] **Step 2: Build**

Run: `make -j 2>&1 | tail -10`
Expected: clean build.

- [ ] **Step 3: Interactive test (per [L060])**

```bash
./build/bin/helix-screen --test -vv -p print_status 2>&1 | tee /tmp/fan_click_test.log
```

Tell the user: "Click on the fan row in the print status panel. The fan control overlay should open. Then go back and confirm the print status panel returns intact."

Wait for confirmation.

- [ ] **Step 4: Inspect log**

Run: `grep "Fans clicked\|fan control overlay" /tmp/fan_click_test.log`
Expected: a single debug line per click; overlay creation logs on first click only.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_panel_print_status.cpp
git commit -m "feat(print_status): wire fan row click to fan control overlay"
```

---

## Task 10: Integration Tests for the Fan Section

**Files:**
- Modify: `tests/unit/test_print_status_fan_section.cpp`

- [ ] **Step 1: Add fixture + binding test**

Append to `tests/unit/test_print_status_fan_section.cpp`:

```cpp
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"
#include "tests/xml_test_fixture.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE_METHOD(XMLTestFixture, "Print status fan row renders for 3 fans",
                 "[print_status][fans]") {
    auto& ps = get_printer_state_for_test();
    std::vector<FanInfo> fans = {
        {"fan", "Part", FanType::PART_COOLING, 75, true, std::nullopt},
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 100, false, std::nullopt},
        {"fan_generic chamber", "Chamber", FanType::GENERIC_FAN, 30, true, std::nullopt}};
    ps.get_fan_state().set_fans_for_test(fans);

    auto& panel = get_global_print_status_panel();
    panel.init_subjects();
    lv_obj_t* root = panel.create(lv_screen_active());
    REQUIRE(root != nullptr);

    // Force the fit subject so the row is laid out (we're testing content,
    // not the fit calc here — that's a separate test below)
    lv_subject_t* fit = lv_xml_get_subject(nullptr, "print_status_fans_fit");
    REQUIRE(fit != nullptr);
    lv_subject_set_int(fit, 1);
    UpdateQueue::instance().process_pending_for_test();

    lv_obj_t* part_speed = lv_obj_find_by_name(root, "part_fan_speed");
    lv_obj_t* hotend_speed = lv_obj_find_by_name(root, "hotend_fan_speed");
    lv_obj_t* aux_speed = lv_obj_find_by_name(root, "aux_fan_speed");
    REQUIRE(part_speed != nullptr);
    REQUIRE(hotend_speed != nullptr);
    REQUIRE(aux_speed != nullptr);

    REQUIRE(std::string(lv_label_get_text(part_speed)) == "75%");
    REQUIRE(std::string(lv_label_get_text(hotend_speed)) == "100%");
    REQUIRE(std::string(lv_label_get_text(aux_speed)) == "30%");

    lv_subject_t* aux_present = lv_xml_get_subject(nullptr, "print_status_aux_fan_present");
    REQUIRE(aux_present != nullptr);
    REQUIRE(lv_subject_get_int(aux_present) == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "Print status fan row hides aux when no aux fan",
                 "[print_status][fans]") {
    auto& ps = get_printer_state_for_test();
    std::vector<FanInfo> fans = {
        {"fan", "Part", FanType::PART_COOLING, 50, true, std::nullopt},
        {"heater_fan hotend_fan", "Hotend", FanType::HEATER_FAN, 100, false, std::nullopt}};
    ps.get_fan_state().set_fans_for_test(fans);

    auto& panel = get_global_print_status_panel();
    panel.init_subjects();
    panel.create(lv_screen_active());
    UpdateQueue::instance().process_pending_for_test();

    lv_subject_t* aux_present = lv_xml_get_subject(nullptr, "print_status_aux_fan_present");
    REQUIRE(aux_present != nullptr);
    REQUIRE(lv_subject_get_int(aux_present) == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "Print status fan row updates on speed change",
                 "[print_status][fans]") {
    auto& ps = get_printer_state_for_test();
    std::vector<FanInfo> fans = {{"fan", "Part", FanType::PART_COOLING, 0, true, std::nullopt}};
    ps.get_fan_state().set_fans_for_test(fans);

    auto& panel = get_global_print_status_panel();
    panel.init_subjects();
    lv_obj_t* root = panel.create(lv_screen_active());
    lv_subject_set_int(lv_xml_get_subject(nullptr, "print_status_fans_fit"), 1);
    UpdateQueue::instance().process_pending_for_test();

    SubjectLifetime lt;
    lv_subject_t* part_subj = ps.get_fan_speed_subject("fan", lt);
    REQUIRE(part_subj != nullptr);
    lv_subject_set_int(part_subj, 88);
    UpdateQueue::instance().process_pending_for_test();

    lv_obj_t* label = lv_obj_find_by_name(root, "part_fan_speed");
    REQUIRE(std::string(lv_label_get_text(label)) == "88%");
}
```

If `process_pending_for_test()`, `get_printer_state_for_test()`, or `XMLTestFixture` accessors don't match this exact spelling, grep `tests/unit/` for an existing test that drains the queue and copy that pattern.

- [ ] **Step 2: Build and run tests**

Run: `make test-run TEST_ARGS="[print_status][fans]" 2>&1 | tail -40`
Expected: 3 cases pass.

- [ ] **Step 3: Run the full test suite to confirm no regressions**

Run: `make test-run 2>&1 | tail -10`
Expected: full suite passes (or at most known-flaky tests; nothing related to fans or print status should fail).

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_print_status_fan_section.cpp
git commit -m "test(print_status): unit tests for fan row binding + aux hide"
```

---

## Final Verification

- [ ] **Step 1: Full build + test cycle**

```bash
make -j && make test-run 2>&1 | tail -20
```

Expected: clean build, all tests pass.

- [ ] **Step 2: End-to-end interactive walkthrough (per [L060])**

```bash
./build/bin/helix-screen --test -vv -p print_status 2>&1 | tee /tmp/fan_row_final.log
```

Tell the user to confirm:

1. Open the print status panel; the fan row appears with Part / Hotend / Aux readouts.
2. Toggle the mock printer to add/remove AMS slots (or use the dev controls) — the fan row should stay visible until the column genuinely can't fit it, then hide.
3. Resize the panel — fan row hides on small breakpoints/heights, reappears on larger.
4. Click the fan row — fan control overlay opens. Back returns to print status with row still intact.
5. Long-running test: leave the panel open through a mock print cycle; speeds update live, icons spin when running.

- [ ] **Step 3: Spec walkthrough**

Re-read `docs/superpowers/specs/2026-05-17-print-status-fan-row-design.md` and confirm every section has been delivered:
- [ ] Layout (horizontal 3-fan row, sister to speed_flow_row)
- [ ] Aux auto-hide
- [ ] Adaptive `print_status_fans_fit` calculation with hysteresis
- [ ] Reuses `classify_primary_fans` helper (extracted, single source of truth)
- [ ] Per-fan `ObserverGuard` + paired `SubjectLifetime` ([L084])
- [ ] `reset()` not `release()` ([L085])
- [ ] Spin animation reuse
- [ ] Click → `fan_control_overlay`
- [ ] No bare threads ([L083])
- [ ] Unit + drift tests pass

- [ ] **Step 4: Optional — note any followups in the scratchpad**

If anything surfaced during implementation that's worth a future session (e.g., extracting more of FanStackWidget's binding pattern), drop a note in `.claude/scratchpad/` and link from `MEMORY.md` if relevant.

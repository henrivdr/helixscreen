// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_logic.h"

#include "wizard_step.h"

#include "../catch_amalgamated.hpp"

#include <vector>

using helix::wizard::StepId;

// ============================================================================
// Default flags (no skips) — baseline behavior
// ============================================================================

TEST_CASE("Default flags: all 13 steps shown", "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    REQUIRE(helix::wizard_calculate_display_total(flags) == 13);
}

TEST_CASE("Default flags: display step numbering is 1-based sequential",
          "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    for (int i = 0; i < 13; ++i) {
        REQUIRE(helix::wizard_calculate_display_step(i, flags) == i + 1);
    }
}

TEST_CASE("Default flags: next_step walks all steps", "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    for (int i = 0; i < 12; ++i) {
        REQUIRE(helix::wizard_next_step(i, flags) == i + 1);
    }
    REQUIRE(helix::wizard_next_step(12, flags) == -1);
}

TEST_CASE("Default flags: prev_step walks all steps backward",
          "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    for (int i = 12; i > 0; --i) {
        REQUIRE(helix::wizard_prev_step(i, flags) == i - 1);
    }
    REQUIRE(helix::wizard_prev_step(0, flags) == -1);
}

// ============================================================================
// Preset mode: skip hardware steps
// ============================================================================

TEST_CASE("Preset mode: skip hardware steps", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.input_shaper = true;
    flags.summary = true;
    // telemetry NOT skipped (shown in preset mode)

    // Steps shown: 0(touch), 1(lang), 3(conn), 12(telemetry) = 4
    REQUIRE(helix::wizard_calculate_display_total(flags) == 4);
    REQUIRE(helix::wizard_calculate_display_step(0, flags) == 1);
    REQUIRE(helix::wizard_calculate_display_step(1, flags) == 2);
    REQUIRE(helix::wizard_calculate_display_step(3, flags) == 3);
    REQUIRE(helix::wizard_calculate_display_step(12, flags) == 4);
}

TEST_CASE("Preset mode: next_step skips hardware",
          "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.input_shaper = true;
    flags.summary = true;

    REQUIRE(helix::wizard_next_step(0, flags) == 1);
    REQUIRE(helix::wizard_next_step(1, flags) == 3);
    REQUIRE(helix::wizard_next_step(3, flags) == 12);
    REQUIRE(helix::wizard_next_step(12, flags) == -1);
}

TEST_CASE("Normal mode: telemetry skipped by default",
          "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    flags.telemetry = true;

    // 12 steps shown (0-11, telemetry skipped)
    REQUIRE(helix::wizard_calculate_display_total(flags) == 12);
    REQUIRE(helix::wizard_next_step(11, flags) == -1); // Summary is last
}

TEST_CASE("Preset mode: prev_step works", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.input_shaper = true;
    flags.summary = true;

    REQUIRE(helix::wizard_prev_step(12, flags) == 3);
    REQUIRE(helix::wizard_prev_step(3, flags) == 1);
    REQUIRE(helix::wizard_prev_step(1, flags) == 0);
    REQUIRE(helix::wizard_prev_step(0, flags) == -1);
}

TEST_CASE("Preset mode: connection also skipped", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    flags.wifi = true;
    flags.connection = true;  // auto-validated
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.input_shaper = true;
    flags.summary = true;
    // telemetry NOT skipped

    // Steps: 0(touch), 1(lang), 12(telemetry) = 3
    REQUIRE(helix::wizard_calculate_display_total(flags) == 3);
    REQUIRE(helix::wizard_next_step(1, flags) == 12);  // lang -> telemetry (skip conn + all hw)
}

// ============================================================================
// Preset skip policy (wizard_preset_plan) — single source of truth for when a
// preset lets the wizard skip hardware steps, decoupled from the first-run
// telemetry fast path.
// ============================================================================

TEST_CASE("Preset plan: no preset means no preset-driven skips",
          "[wizard][step_logic][preset]") {
    auto plan = helix::wizard_preset_plan(/*has_preset=*/false, /*printer_count=*/1);
    REQUIRE_FALSE(plan.skip_hardware);
    REQUIRE_FALSE(plan.first_run);

    // No preset, even on a multi-printer config, still skips nothing preset-driven.
    auto plan2 = helix::wizard_preset_plan(false, 3);
    REQUIRE_FALSE(plan2.skip_hardware);
    REQUIRE_FALSE(plan2.first_run);
}

TEST_CASE("Preset plan: first printer with preset is the first-run fast path",
          "[wizard][step_logic][preset]") {
    auto plan = helix::wizard_preset_plan(/*has_preset=*/true, /*printer_count=*/1);
    REQUIRE(plan.skip_hardware);
    REQUIRE(plan.first_run); // skips summary + shows telemetry opt-in

    // A zero count (config not yet listing the active printer) is treated as
    // first-run, not as "many printers".
    auto plan0 = helix::wizard_preset_plan(true, 0);
    REQUIRE(plan0.skip_hardware);
    REQUIRE(plan0.first_run);
}

TEST_CASE("Preset plan: SECOND printer with preset skips hardware but NOT first-run",
          "[wizard][step_logic][preset][regression]") {
    // Regression: a known printer added as the 2nd+ printer (e.g. a Creality K2
    // Plus next to an existing Voron) was force-marched through manual heater/fan/
    // sensor mapping even though its preset fully configured the hardware. The
    // hardware pickers must be skipped for any printer with a preset; only the
    // one-time telemetry fast path stays gated to the first printer.
    auto plan = helix::wizard_preset_plan(/*has_preset=*/true, /*printer_count=*/2);
    REQUIRE(plan.skip_hardware);   // <-- the fix: redundant hardware steps are skipped
    REQUIRE_FALSE(plan.first_run); // <-- telemetry must NOT re-fire; summary stays shown

    auto plan5 = helix::wizard_preset_plan(true, 5);
    REQUIRE(plan5.skip_hardware);
    REQUIRE_FALSE(plan5.first_run);
}

TEST_CASE("Preset plan: secondary-printer flags navigate connection -> summary -> done",
          "[wizard][step_logic][preset]") {
    // Build the skip flags a secondary preset printer produces and confirm the
    // wizard walks straight to the summary, then finishes (no telemetry).
    auto plan = helix::wizard_preset_plan(true, 2);
    REQUIRE(plan.skip_hardware);
    REQUIRE_FALSE(plan.first_run);

    helix::WizardSkipFlags flags{};
    // first three steps are skipped for any subsequent printer
    flags.touch_cal = true;
    flags.language = true;
    flags.wifi = true;
    // preset covers hardware (steps 4-10)
    flags.printer_identify = plan.skip_hardware;
    flags.heater_select = plan.skip_hardware;
    flags.fan_select = plan.skip_hardware;
    flags.ams = plan.skip_hardware;
    flags.led = plan.skip_hardware;
    flags.filament = plan.skip_hardware;
    flags.input_shaper = plan.skip_hardware;
    // not first-run: summary shown, telemetry skipped
    flags.summary = plan.first_run;
    flags.telemetry = !plan.first_run;

    REQUIRE(helix::wizard_next_step(3, flags) == 11); // connection -> summary
    REQUIRE(helix::wizard_next_step(11, flags) == -1); // summary -> done (telemetry skipped)
    REQUIRE(helix::wizard_calculate_display_total(flags) == 2); // connection + summary
}

// ============================================================================
// Id-based pure navigation over the step registry (StepId + StepSkip vector).
// ============================================================================

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
    for (auto& s : v) if (s.id == StepId::HeaterSelect || s.id == StepId::AmsIdentify ||
                          s.id == StepId::InputShaper) s.skipped = true;
    REQUIRE(helix::wizard_next(StepId::PrinterIdentify, v) == StepId::FanSelect);
    REQUIRE(helix::wizard_next(StepId::FanSelect, v) == StepId::LedSelect);
    REQUIRE(helix::wizard_next(StepId::FilamentSensor, v) == StepId::Summary);
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

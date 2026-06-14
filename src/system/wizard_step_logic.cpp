// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_logic.h"

namespace helix {

// Total steps without any skips: 13 (steps 0-12)
static constexpr int TOTAL_STEPS = 13;

/// Check if a given internal step is skipped
static bool is_step_skipped(int step, const WizardSkipFlags& skips) {
    switch (step) {
    case 0:
        return skips.touch_cal;
    case 1:
        return skips.language;
    case 2:
        return skips.wifi;
    case 3:
        return skips.connection;
    case 4:
        return skips.printer_identify;
    case 5:
        return skips.heater_select;
    case 6:
        return skips.fan_select;
    case 7:
        return skips.ams;
    case 8:
        return skips.led;
    case 9:
        return skips.filament;
    case 10:
        return skips.input_shaper;
    case 11:
        return skips.summary;
    case 12:
        return skips.telemetry;
    default:
        return false;
    }
}

int wizard_calculate_display_step(int internal_step, const WizardSkipFlags& skips) {
    int display = 1; // 1-based
    for (int i = 0; i < internal_step; ++i) {
        if (!is_step_skipped(i, skips)) {
            display++;
        }
    }
    return display;
}

int wizard_calculate_display_total(const WizardSkipFlags& skips) {
    int total = 0;
    for (int i = 0; i < TOTAL_STEPS; ++i) {
        if (!is_step_skipped(i, skips)) {
            total++;
        }
    }
    return total;
}

int wizard_next_step(int current, const WizardSkipFlags& skips) {
    for (int i = current + 1; i < TOTAL_STEPS; ++i) {
        if (!is_step_skipped(i, skips)) {
            return i;
        }
    }
    return -1; // At end
}

int wizard_prev_step(int current, const WizardSkipFlags& skips) {
    for (int i = current - 1; i >= 0; --i) {
        if (!is_step_skipped(i, skips)) {
            return i;
        }
    }
    return -1; // At beginning
}

WizardPresetPlan wizard_preset_plan(bool has_preset, int printer_count) {
    WizardPresetPlan plan;
    // A complete preset configures the hardware for any printer it applies to,
    // so the hardware-pick steps are redundant regardless of printer order.
    plan.skip_hardware = has_preset;
    // The first-run fast path additionally skips the summary and shows the
    // one-time telemetry opt-in — gated to the initial printer so telemetry
    // never re-prompts when adding subsequent printers.
    plan.first_run = has_preset && printer_count <= 1;
    return plan;
}

} // namespace helix

// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_logic.h"

#include "wizard_step.h" // helix::wizard::StepId

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

// ============================================================================
// Id-based pure navigation over the step registry.
// ============================================================================

namespace {

// Index of `current` in the vector (matched by id), or -1 if absent — treated
// as "before the first entry" by the navigation helpers.
int index_of(wizard::StepId current, const std::vector<StepSkip>& steps) {
    for (size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].id == current) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

int wizard_visible_count(const std::vector<StepSkip>& steps) {
    int count = 0;
    for (const auto& s : steps) {
        if (!s.skipped) {
            count++;
        }
    }
    return count;
}

int wizard_display_number(wizard::StepId current, const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps);
    int upper = (idx < 0) ? 0 : idx;
    int display = 1; // 1-based
    for (int i = 0; i < upper; ++i) {
        if (!steps[i].skipped) {
            display++;
        }
    }
    return display;
}

std::optional<wizard::StepId> wizard_next(wizard::StepId current,
                                          const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps); // -1 => before first, start scan at 0
    for (int i = idx + 1; i < static_cast<int>(steps.size()); ++i) {
        if (!steps[i].skipped) {
            return steps[i].id;
        }
    }
    return std::nullopt;
}

std::optional<wizard::StepId> wizard_prev(wizard::StepId current,
                                          const std::vector<StepSkip>& steps) {
    int idx = index_of(current, steps);
    if (idx < 0) {
        idx = static_cast<int>(steps.size()); // not found => scan whole list backward
    }
    for (int i = idx - 1; i >= 0; --i) {
        if (!steps[i].skipped) {
            return steps[i].id;
        }
    }
    return std::nullopt;
}

bool wizard_is_last(wizard::StepId current, const std::vector<StepSkip>& steps) {
    return wizard_next(current, steps) == std::nullopt;
}

} // namespace helix

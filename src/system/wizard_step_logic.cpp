// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_logic.h"

#include "wizard_step.h" // helix::wizard::StepId

namespace helix {

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

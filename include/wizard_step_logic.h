// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <vector>

namespace helix {

/// Preset-driven wizard skip policy, derived purely from whether a complete
/// preset is applied and how many printers are configured. Single source of
/// truth so the LVGL wizard and tests agree, and so the two concerns can't drift:
///   - skip_hardware: the preset already configures heater/fan/AMS/LED/filament/
///     input-shaper, so those pickers are redundant. True for ANY printer with a
///     preset — the first one or a later one added via the printer manager.
///   - first_run: additionally skip the summary and show the one-time telemetry
///     opt-in. First configured printer only — telemetry is a global, one-time
///     prompt, so it must not re-fire when adding subsequent printers.
struct WizardPresetPlan {
    bool skip_hardware = false;
    bool first_run = false;
};
WizardPresetPlan wizard_preset_plan(bool has_preset, int printer_count);

// ============================================================================
// Id-based pure navigation over the step registry. Operates on a vector of
// {StepId, skipped} entries — the registry-driven representation. No LVGL;
// fully testable. This is the sole navigation API; the wizard and tests both
// drive it.
// ============================================================================

namespace wizard {
enum class StepId;
}

/// One registry entry's navigation state: its id and whether it is skipped.
struct StepSkip {
    wizard::StepId id;
    bool skipped;
};

/// Count of non-skipped entries.
int wizard_visible_count(const std::vector<StepSkip>&);

/// 1-based display number for `current`: 1 + number of visible entries strictly
/// before it.
int wizard_display_number(wizard::StepId current, const std::vector<StepSkip>&);

/// First non-skipped entry after `current`, or nullopt if none.
std::optional<wizard::StepId> wizard_next(wizard::StepId, const std::vector<StepSkip>&);

/// First non-skipped entry before `current`, or nullopt if none.
std::optional<wizard::StepId> wizard_prev(wizard::StepId, const std::vector<StepSkip>&);

/// True if there is no visible entry after `current`.
bool wizard_is_last(wizard::StepId, const std::vector<StepSkip>&);

} // namespace helix

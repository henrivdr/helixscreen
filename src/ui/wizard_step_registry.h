// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

#include "wizard_step.h"        // helix::wizard::Step, StepId, StepContext
#include "wizard_step_logic.h"  // helix::StepSkip

namespace helix {
namespace wizard {

/// Ordered list of all wizard steps, indexed by StepId (0..kStepCount-1).
/// Built lazily on first call and cached for the process lifetime.
const std::vector<Step*>& steps();

/// Step for the given id, or nullptr if the id is out of range.
Step* step_by_id(StepId id);

/// Build a StepContext from current global state (Config + Moonraker API).
StepContext build_context();

/// Per-step skip decisions in StepId order, given a context.
std::vector<helix::StepSkip> skip_vector(const StepContext& ctx);

} // namespace wizard
} // namespace helix

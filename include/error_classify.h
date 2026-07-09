// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "error_event.h"

#include <optional>
#include <string>

namespace helix::error_classify {

/// Classify a single raw gcode-response line into an ErrorEvent.
/// Returns nullopt for non-error lines (`//`, `ok`, status, action:prompt).
/// Pure: all environment comes via `ctx`. Reuses
/// GcodeErrorRouter::clean_error_text for translation.
std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx);

} // namespace helix::error_classify

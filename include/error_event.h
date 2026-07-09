// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix {

enum class ErrorSeverity { INFO, WARNING, CRITICAL };

enum class ErrorSource {
    GENERIC,
    KLIPPER,
    HEATER,
    AFC,
    CFS,
    IFS,
    QIDI,
    HAPPY_HARE,
    SNAPMAKER,
    ACE,
    TOOLCHANGER
};

/// A recovery action offered alongside a CRITICAL error. In L0 these are
/// populated only by classifiers that already know a one-tap fix (the
/// migrated CFS key840 case); per-backend smart actions arrive in L1.
struct RecoveryAction {
    std::string label;   ///< Button label, e.g. "Unload"
    std::string gcode;   ///< G-code to run on tap
    std::string log_tag; ///< spdlog tag on tap
    std::string style;   ///< "" (neutral) | "primary" | "danger" — maps to PromptButton.color
};

/// Result of classifying one gcode-response line. Produced by the pure
/// `error_classify::classify()`; consumed by the router's presenter.
struct ErrorEvent {
    ErrorSource source = ErrorSource::GENERIC;
    ErrorSeverity severity = ErrorSeverity::WARNING;
    std::string title;  ///< short, already-translated; "" -> presenter default
    std::string detail; ///< FULL, untruncated, translated message text
    std::string code;   ///< Klipper error code if any ("key840"), else ""
    std::vector<RecoveryAction> recovery_actions;
    bool sticky = false;
};

/// Inputs the classifier needs beyond the raw line. Kept as a plain struct
/// so the classifier stays pure and unit-testable without globals.
struct ClassifyContext {
    bool is_paused = false;   ///< print currently paused (pause_resume.is_paused)
    bool is_printing = false; ///< print active (print_stats.state == "printing")
};

} // namespace helix

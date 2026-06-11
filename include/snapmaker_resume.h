// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pause_cause.h"

#include <string>
#include <vector>

namespace helix {

/// Terminal-cause patterns for the Snapmaker U1.
///
/// Hardware-verified on a physical U1 (prestonbrown/helixscreen#991):
///   - Dirty bed:  exception {id:532, code:1, message:"detected dirty bed"} — Terminal.
///   - Runout:     exception {id:523, code:0, message:"e1_filament runout"} — Recoverable.
/// Two matchers cover dirty-bed by either id or message text. We deliberately do
/// NOT set require_sdcard_inactive: runout ALSO clears virtual_sdcard.is_active,
/// so sdcard state cannot discriminate terminal-vs-recoverable here.
std::vector<TerminalMatcher> snapmaker_terminal_matchers();

/// Build the per-extruder filament-config re-assertion gcode line emitted
/// before RESUME (Snapmaker firmware drops FILAMENT_TYPE/VENDOR on sensor
/// loss). Returns "" when material or brand is empty, so we never emit a
/// half-populated SET_PRINT_FILAMENT_CONFIG; the caller logs a warning.
/// Trailing newline included for chain concatenation.
///
/// NOTE (prestonbrown/helixscreen#991): uses the confirmed manual gcode arg
/// names FILAMENT_TYPE / VENDOR (Discord 2026-06). The REST /filament_detect/set
/// path uses MAIN_TYPE — a different interface; verify casing on-device.
[[nodiscard]] std::string snapmaker_filament_config_gcode(int extruder, const std::string& material,
                                                          const std::string& brand);

/// Post-resume no-op backstop predicate: true => RESUME silently no-op'd
/// (print still paused with SD playback inactive) => offer restart.
[[nodiscard]] bool snapmaker_resume_noop_detected(bool is_paused, bool sdcard_active);

/// Verdict for one tick of the heating-aware post-resume backstop.
enum class ResumeBackstopVerdict {
    ResumedOk,   ///< Print un-paused / SD active again — RESUME succeeded.
    KeepWaiting, ///< Still settling (reheating or within the move-tail floor).
    NoOpRestart, ///< Genuinely stuck — surface the "restart required" modal.
};

/// Decide what a post-resume backstop sampling tick should do.
///
/// is_paused / sdcard_active: current print state. extruder_heating: the active
/// nozzle is below target by a margin with a real target set (RESUME's M109 is
/// still blocking). elapsed_ms: time since the backstop was armed. max_wait_ms:
/// give-up ceiling. settle_floor_ms: minimum wait before declaring a no-op once
/// heating has finished (covers the post-heat move tail).
///
/// Pure: no LVGL, threading, or singletons — unit-testable in isolation. Fixes
/// the false "restart required" modal during a legitimate cold-nozzle resume,
/// where a fixed 15s timeout fired while a 37s reheat-resume was still running.
[[nodiscard]] ResumeBackstopVerdict snapmaker_resume_backstop_verdict(bool is_paused,
                                                                      bool sdcard_active,
                                                                      bool extruder_heating,
                                                                      int elapsed_ms, int max_wait_ms,
                                                                      int settle_floor_ms);

} // namespace helix

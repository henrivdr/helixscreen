// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_resume.h"

namespace helix {

std::vector<TerminalMatcher> snapmaker_terminal_matchers() {
    // Hardware-verified on a physical Snapmaker U1 (#991): dirty-bed aborts with
    // exception {id:532, code:1, message:"detected dirty bed"} — Terminal. Match
    // by message substring OR by exception id so either signal alone suffices.
    // NOT gated on sdcard-inactive: runout (id 523) also deactivates the SD, so
    // sdcard state cannot discriminate terminal-vs-recoverable here.
    return {
        TerminalMatcher{/*message_substr=*/"dirty bed", /*exception_id=*/-1,
                        /*require_sdcard_inactive=*/false},
        TerminalMatcher{/*message_substr=*/"", /*exception_id=*/532,
                        /*require_sdcard_inactive=*/false},
    };
}

std::string snapmaker_filament_config_gcode(int extruder, const std::string& material,
                                            const std::string& brand,
                                            const std::string& sub_type) {
    // #991: the firmware's cmd_SET_PRINT_FILAMENT_CONFIG REQUIRES FILAMENT_SUBTYPE
    // (and VENDOR) whenever FILAMENT_TYPE is given — omitting it raises
    // "[print_task_config] filament_config, incomplete parameters" and aborts the
    // post-runout recovery chain. All three must be present together, so skip
    // (return "") if any is empty; the caller logs a warning and the re-assert is
    // simply not sent (it is best-effort, not required for the rest of recovery).
    if (material.empty() || brand.empty() || sub_type.empty()) {
        return "";
    }
    return "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='" + std::to_string(extruder) +
           "' FILAMENT_TYPE='" + material + "' VENDOR='" + brand + "' FILAMENT_SUBTYPE='" +
           sub_type + "'\n";
}

bool snapmaker_resume_noop_detected(bool is_paused, bool sdcard_active) {
    return is_paused && !sdcard_active;
}

ResumeBackstopVerdict snapmaker_resume_backstop_verdict(bool is_paused, bool sdcard_active,
                                                        bool extruder_heating, int elapsed_ms,
                                                        int max_wait_ms, int settle_floor_ms) {
    if (!is_paused || sdcard_active) {
        return ResumeBackstopVerdict::ResumedOk; // print un-paused → success
    }
    if (elapsed_ms >= max_wait_ms) {
        return ResumeBackstopVerdict::NoOpRestart; // waited long enough, still stuck
    }
    if (extruder_heating) {
        return ResumeBackstopVerdict::KeepWaiting; // M109 reheat in progress
    }
    if (elapsed_ms < settle_floor_ms) {
        return ResumeBackstopVerdict::KeepWaiting; // allow post-heat move tail
    }
    return ResumeBackstopVerdict::NoOpRestart; // genuinely stuck, not heating
}

} // namespace helix

// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/pause_cause.h"
#include "../../include/snapmaker_resume.h"

#include "../catch_amalgamated.hpp"

using helix::classify_pause;
using helix::PauseCause;
using helix::PauseSignals;
using helix::snapmaker_filament_config_gcode;
using helix::snapmaker_resume_backstop_verdict;
using helix::snapmaker_resume_noop_detected;
using helix::snapmaker_terminal_matchers;
using helix::ResumeBackstopVerdict;

TEST_CASE("snapmaker_terminal_matchers: dirty-bed by id 532 => Terminal", "[pause][snapmaker]") {
    PauseSignals s;
    s.exception_id = 532;
    s.message = "detected dirty bed";
    REQUIRE(classify_pause(s, snapmaker_terminal_matchers()) == PauseCause::Terminal);
}

TEST_CASE("snapmaker_terminal_matchers: dirty-bed by message => Terminal", "[pause][snapmaker]") {
    PauseSignals s;
    s.exception_id = -1; // id missing, only message present
    s.message = "detected dirty bed";
    REQUIRE(classify_pause(s, snapmaker_terminal_matchers()) == PauseCause::Terminal);
}

TEST_CASE("snapmaker_terminal_matchers: runout (id 523) => Recoverable", "[pause][snapmaker]") {
    // Runout also deactivates virtual_sdcard, so the matchers must NOT key on
    // sdcard state — id 523 / "runout" must classify Recoverable.
    PauseSignals s;
    s.exception_id = 523;
    s.message = "e1_filament runout";
    s.sdcard_active = false;
    s.runout_tripped = true;
    REQUIRE(classify_pause(s, snapmaker_terminal_matchers()) == PauseCause::Recoverable);
}

TEST_CASE("snapmaker_terminal_matchers: user pause (empty signals) => Unknown",
          "[pause][snapmaker]") {
    PauseSignals s; // empty message, exception -1, sdcard active, no runout
    REQUIRE(classify_pause(s, snapmaker_terminal_matchers()) == PauseCause::Unknown);
}

TEST_CASE("snapmaker_filament_config_gcode: builds command for populated slot",
          "[pause][snapmaker]") {
    REQUIRE(
        snapmaker_filament_config_gcode(0, "PLA", "Snapmaker") ==
        "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='0' FILAMENT_TYPE='PLA' VENDOR='Snapmaker'\n");
}

TEST_CASE("snapmaker_filament_config_gcode: empty material => skip (empty string)",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(0, "", "Snapmaker").empty());
}

TEST_CASE("snapmaker_filament_config_gcode: empty brand => skip (empty string)",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(1, "PETG", "").empty());
}

TEST_CASE("snapmaker_filament_config_gcode: uses the given non-zero extruder index",
          "[pause][snapmaker]") {
    REQUIRE(snapmaker_filament_config_gcode(2, "ABS", "eSUN") ==
            "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER='2' FILAMENT_TYPE='ABS' VENDOR='eSUN'\n");
}

TEST_CASE("snapmaker_resume_noop_detected: paused + SD inactive => true", "[pause][snapmaker]") {
    REQUIRE(snapmaker_resume_noop_detected(/*is_paused=*/true, /*sdcard_active=*/false));
}

TEST_CASE("snapmaker_resume_noop_detected: otherwise => false", "[pause][snapmaker]") {
    REQUIRE_FALSE(snapmaker_resume_noop_detected(false, false));
    REQUIRE_FALSE(snapmaker_resume_noop_detected(true, true));
    REQUIRE_FALSE(snapmaker_resume_noop_detected(false, true));
}

// ---------------------------------------------------------------------------
// snapmaker_resume_backstop_verdict — heating-aware post-resume backstop (#991)
// ---------------------------------------------------------------------------
namespace {
constexpr int kMaxWait = 150000;
constexpr int kSettle = 20000;
} // namespace

TEST_CASE("backstop verdict: un-paused => ResumedOk", "[pause][snapmaker][backstop]") {
    // is_paused=false means the print resumed successfully.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/false, /*sdcard_active=*/false,
                                              /*extruder_heating=*/false, /*elapsed=*/5000, kMaxWait,
                                              kSettle) == ResumeBackstopVerdict::ResumedOk);
}

TEST_CASE("backstop verdict: sdcard active => ResumedOk", "[pause][snapmaker][backstop]") {
    // Paused flag may lag, but SD playback active means the print is running.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/true, /*sdcard_active=*/true,
                                              /*extruder_heating=*/false, /*elapsed=*/5000, kMaxWait,
                                              kSettle) == ResumeBackstopVerdict::ResumedOk);
}

TEST_CASE("backstop verdict: heating within window => KeepWaiting", "[pause][snapmaker][backstop]") {
    // M109 reheat still blocking RESUME — give it more time.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/true, /*sdcard_active=*/false,
                                              /*extruder_heating=*/true, /*elapsed=*/30000, kMaxWait,
                                              kSettle) == ResumeBackstopVerdict::KeepWaiting);
}

TEST_CASE("backstop verdict: not heating but within settle floor => KeepWaiting",
          "[pause][snapmaker][backstop]") {
    // Allow the post-heat move tail before declaring a no-op.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/true, /*sdcard_active=*/false,
                                              /*extruder_heating=*/false, /*elapsed=*/10000,
                                              kMaxWait, kSettle) ==
            ResumeBackstopVerdict::KeepWaiting);
}

TEST_CASE("backstop verdict: past max_wait still paused => NoOpRestart",
          "[pause][snapmaker][backstop]") {
    // Waited long enough (even while heating) — give up and surface restart.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/true, /*sdcard_active=*/false,
                                              /*extruder_heating=*/true, /*elapsed=*/kMaxWait,
                                              kMaxWait, kSettle) ==
            ResumeBackstopVerdict::NoOpRestart);
}

TEST_CASE("backstop verdict: not heating past settle within window => NoOpRestart",
          "[pause][snapmaker][backstop]") {
    // Genuinely stuck: past the settle floor, not heating, still paused.
    REQUIRE(snapmaker_resume_backstop_verdict(/*is_paused=*/true, /*sdcard_active=*/false,
                                              /*extruder_heating=*/false, /*elapsed=*/25000,
                                              kMaxWait, kSettle) ==
            ResumeBackstopVerdict::NoOpRestart);
}

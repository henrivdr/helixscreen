// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/pause_cause.h"

#include "../catch_amalgamated.hpp"

using helix::classify_pause;
using helix::PauseCause;
using helix::PauseSignals;
using helix::TerminalMatcher;

TEST_CASE("classify_pause: empty matcher list is never Terminal", "[pause][classify]") {
    PauseSignals s;
    s.message = "Dirty bed detected";
    s.sdcard_active = false;
    REQUIRE(classify_pause(s, {}) == PauseCause::Recoverable);
}

TEST_CASE("classify_pause: message substring match => Terminal", "[pause][classify]") {
    PauseSignals s;
    s.message = "Dirty bed detected";
    s.sdcard_active = false;
    std::vector<TerminalMatcher> m = {{"dirty", -1, true}};
    REQUIRE(classify_pause(s, m) == PauseCause::Terminal);
}

TEST_CASE("classify_pause: substring match is case-insensitive", "[pause][classify]") {
    PauseSignals s;
    s.message = "DIRTY BED";
    s.sdcard_active = false;
    std::vector<TerminalMatcher> m = {{"dirty", -1, true}};
    REQUIRE(classify_pause(s, m) == PauseCause::Terminal);
}

TEST_CASE("classify_pause: require_sdcard_inactive gates the match", "[pause][classify]") {
    PauseSignals s;
    s.message = "Dirty bed";
    s.sdcard_active = true; // SD still active => not terminal
    std::vector<TerminalMatcher> m = {{"dirty", -1, true}};
    REQUIRE(classify_pause(s, m) == PauseCause::Recoverable);
}

TEST_CASE("classify_pause: exception id match => Terminal", "[pause][classify]") {
    PauseSignals s;
    s.exception_id = 999;
    std::vector<TerminalMatcher> m = {{"", 999, false}};
    REQUIRE(classify_pause(s, m) == PauseCause::Terminal);
}

TEST_CASE("classify_pause: runout is Recoverable even with terminal matchers",
          "[pause][classify]") {
    PauseSignals s;
    s.exception_id = 523;
    s.runout_tripped = true;
    std::vector<TerminalMatcher> m = {{"dirty", -1, true}};
    REQUIRE(classify_pause(s, m) == PauseCause::Recoverable);
}

TEST_CASE("classify_pause: no signal at all => Unknown", "[pause][classify]") {
    PauseSignals s; // empty message, exception -1, sdcard active, no runout
    REQUIRE(classify_pause(s, {}) == PauseCause::Unknown);
}

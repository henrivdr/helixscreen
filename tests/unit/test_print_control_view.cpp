// tests/unit/test_print_control_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_view.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::ui::compute_control_button_view;
using helix::ui::PendingAction;

TEST_CASE("control view: printing shows enabled Pause", "[print_control_view]") {
    auto v = compute_control_button_view(PrintJobState::PRINTING, PendingAction::None,
                                         /*pause*/ true, /*resume*/ true, /*cancel*/ true);
    REQUIRE(std::string(v.primary_label) == "Pause");
    REQUIRE(v.primary_enabled);
    REQUIRE(v.stop_enabled);
}

TEST_CASE("control view: paused shows enabled Resume (play icon)", "[print_control_view]") {
    auto v =
        compute_control_button_view(PrintJobState::PAUSED, PendingAction::None, true, true, true);
    REQUIRE(std::string(v.primary_label) == "Resume");
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x90\x8A"); // play
    REQUIRE(v.primary_enabled);
}

TEST_CASE("control view: idle disables both buttons", "[print_control_view]") {
    for (auto s : {PrintJobState::STANDBY, PrintJobState::COMPLETE, PrintJobState::CANCELLED,
                   PrintJobState::ERROR}) {
        auto v = compute_control_button_view(s, PendingAction::None, true, true, true);
        REQUIRE_FALSE(v.primary_enabled);
        REQUIRE_FALSE(v.stop_enabled);
    }
}

TEST_CASE("control view: pending Pausing -> hourglass, disabled, transitional label",
          "[print_control_view]") {
    auto v = compute_control_button_view(PrintJobState::PRINTING, PendingAction::Pausing, true,
                                         true, true);
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x94\x9F"); // hourglass
    REQUIRE(std::string(v.primary_label) == "Pausing...");
    REQUIRE_FALSE(v.primary_enabled);
    REQUIRE(v.stop_enabled);
}

TEST_CASE("control view: pending Resuming -> hourglass + Resuming label", "[print_control_view]") {
    auto v = compute_control_button_view(PrintJobState::PAUSED, PendingAction::Resuming, true, true,
                                         true);
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x94\x9F");
    REQUIRE(std::string(v.primary_label) == "Resuming...");
    REQUIRE_FALSE(v.primary_enabled);
}

TEST_CASE("control view: missing macro slot disables primary", "[print_control_view]") {
    auto v = compute_control_button_view(PrintJobState::PRINTING, PendingAction::None,
                                         /*pause*/ false, /*resume*/ true, /*cancel*/ true);
    REQUIRE_FALSE(v.primary_enabled);
    auto v2 = compute_control_button_view(PrintJobState::PAUSED, PendingAction::None,
                                          /*pause*/ true, /*resume*/ false, /*cancel*/ true);
    REQUIRE_FALSE(v2.primary_enabled);
    auto v3 = compute_control_button_view(PrintJobState::PRINTING, PendingAction::None, true, true,
                                          /*cancel*/ false);
    REQUIRE_FALSE(v3.stop_enabled);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "action_prompt_manager.h" // helix::PromptData / helix::PromptButton
#include "error_event.h"
#include "gcode_error_router.h"

#include "catch_amalgamated.hpp"

using helix::ErrorEvent;
using helix::ErrorSeverity;

TEST_CASE("CRITICAL with no actions -> MODAL", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::MODAL);
}
TEST_CASE("CRITICAL with a recovery action -> MODAL_WITH_RECOVER", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    e.recovery_actions.push_back({"Reset CFS", "BOX_ERROR_CLEAR", "t"});
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::MODAL_WITH_RECOVER);
}
TEST_CASE("WARNING with recover action -> TOAST_WITH_RECOVER", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::WARNING;
    e.recovery_actions.push_back({"Recover", "", "t"});
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::TOAST_WITH_RECOVER);
}
TEST_CASE("WARNING no actions -> TOAST", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::WARNING;
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::TOAST);
}
TEST_CASE("INFO -> NONE", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::INFO;
    REQUIRE(helix::decide_presentation(e) == helix::PresentAs::NONE);
}

TEST_CASE("build_recovery_prompt maps actions to buttons", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "Possible filament break at the toolhead.";
    e.recovery_actions.push_back({"Resume", "RESUME", "afc::resume", "primary"});
    e.recovery_actions.push_back({"Unload", "TOOL_UNLOAD", "afc::tool_unload", ""});
    e.recovery_actions.push_back({"Recover", "AFC_RESET", "afc::reset", "danger"});

    helix::PromptData p = helix::build_recovery_prompt(e);

    REQUIRE(p.title == "Toolhead jam");
    REQUIRE(p.text_lines.size() == 1);
    REQUIRE(p.text_lines[0] == "Possible filament break at the toolhead.");
    REQUIRE(p.buttons.size() == 3);
    REQUIRE(p.buttons[0].label == "Resume");
    REQUIRE(p.buttons[0].gcode == "RESUME");
    REQUIRE(p.buttons[0].color == "primary");
    REQUIRE(p.buttons[1].label == "Unload");
    REQUIRE(p.buttons[1].color.empty());    // neutral
    REQUIRE(p.buttons[2].color == "error"); // "danger" -> "error"
}

TEST_CASE("build_recovery_prompt falls back to default title", "[error-center][routing]") {
    ErrorEvent e;
    e.severity = ErrorSeverity::CRITICAL;
    e.detail = "x";
    e.recovery_actions.push_back({"Reset CFS", "BOX_ERROR_CLEAR", "t", ""});
    helix::PromptData p = helix::build_recovery_prompt(e);
    REQUIRE_FALSE(p.title.empty()); // non-empty default title
    REQUIRE(p.buttons.size() == 1);
}

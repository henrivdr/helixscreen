// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "gcode_error_router.h"
#include "error_event.h"

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

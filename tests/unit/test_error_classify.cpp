// SPDX-License-Identifier: GPL-3.0-or-later
#include "catch_amalgamated.hpp"
#include "error_event.h"

using helix::ErrorEvent;
using helix::ErrorSeverity;
using helix::ErrorSource;

TEST_CASE("ErrorEvent defaults are safe", "[error-center][model]") {
    ErrorEvent e;
    REQUIRE(e.severity == ErrorSeverity::WARNING);   // conservative default
    REQUIRE(e.source == ErrorSource::GENERIC);
    REQUIRE(e.title.empty());
    REQUIRE(e.detail.empty());
    REQUIRE(e.code.empty());
    REQUIRE(e.recovery_actions.empty());
    REQUIRE_FALSE(e.sticky);
}

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

#include "error_classify.h"
using helix::ClassifyContext;
using helix::error_classify::classify;

TEST_CASE("uncoded jam !! while paused is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx; ctx.is_paused = true;
    auto e = classify("!! Toolhead runout detected by tool_end sensor, but upstream "
                      "sensors still detect filament. Possible filament break or jam "
                      "at the toolhead. Please clear the jam and reload filament "
                      "manually, then resume the print.", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::GENERIC);
    REQUIRE(e->code.empty());
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos);
    REQUIRE(e->detail.size() > 80);
}

TEST_CASE("uncoded !! while idle is WARNING", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("!! Timer too close", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
}

TEST_CASE("CFS key8xx is CRITICAL", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key849","msg":"retract failed","values":[1]})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->code == "key849");
}

TEST_CASE("key840 carries a recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key840","msg":"box switch state error"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->code == "key840");
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode == "BOX_ERROR_CLEAR");
}

TEST_CASE("key298 is WARNING with recovery action", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify(R"(!! {"code":"key298","msg":"klipper_mcu shutdown"})", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->source == helix::ErrorSource::CFS);
    REQUIRE(e->recovery_actions.size() == 1);
    REQUIRE(e->recovery_actions[0].gcode.empty());
}

TEST_CASE("Error: command error is WARNING/KLIPPER", "[error-center][classify]") {
    ClassifyContext ctx;
    auto e = classify("Error: Must home axis first", ctx);
    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::WARNING);
    REQUIRE(e->source == helix::ErrorSource::KLIPPER);
}

TEST_CASE("non-error line yields nullopt", "[error-center][classify]") {
    ClassifyContext ctx;
    REQUIRE_FALSE(classify("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(classify("ok T:210", ctx).has_value());
}

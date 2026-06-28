// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_switcher_widget.cpp
 * @brief Tests for ToolSwitcherWidget registration and metadata
 *
 * Verifies that the tool_switcher widget is registered correctly in the
 * panel widget registry with expected metadata and hardware gating.
 */

#include "panel_widget_registry.h"
#include "tool_state.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("ToolSwitcherWidget: widget def exists in registry", "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    SECTION("has correct id") {
        REQUIRE(std::strcmp(def->id, "tool_switcher") == 0);
    }

    SECTION("has display name") {
        REQUIRE(def->display_name != nullptr);
        REQUIRE(std::strlen(def->display_name) > 0);
    }

    SECTION("has icon") {
        REQUIRE(def->icon != nullptr);
        REQUIRE(std::strlen(def->icon) > 0);
    }

    SECTION("has description") {
        REQUIRE(def->description != nullptr);
        REQUIRE(std::strlen(def->description) > 0);
    }
}

TEST_CASE("ToolSwitcherWidget: no hardware gate (visible to all printers)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

TEST_CASE("ToolSwitcherWidget: supports scaling from 1x1 to 2x2", "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);

    // Default is 1x1 (compact)
    REQUIRE(def->colspan == 1);
    REQUIRE(def->rowspan == 1);

    // Can scale up to 2x2
    REQUIRE(def->effective_max_colspan() == 2);
    REQUIRE(def->effective_max_rowspan() == 2);

    // Minimum is 1x1
    REQUIRE(def->effective_min_colspan() == 1);
    REQUIRE(def->effective_min_rowspan() == 1);

    REQUIRE(def->is_scalable());
}

TEST_CASE("ToolSwitcherWidget: not enabled by default (requires multi-tool)",
          "[tool_switcher][panel_widget]") {
    const auto* def = find_widget_def("tool_switcher");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->default_enabled);
}

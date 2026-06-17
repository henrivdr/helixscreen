// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for AmsBackendSnapmaker::build_preprint_gcode — the pure builder
// that produces the firmware-native print_task_config command sequence
// (SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS) emitted before
// PRINT_START on the Snapmaker U1.
//
// The U1 is a true toolchanger with 4 identical physical heads (0-3); logical
// gcode tools span 0-31. Firmware's default map is [0,1,2,3,0,0,...], so any
// extended tool (4-31) without an explicit user remap falls to physical head 0.
//
// The function is intentionally network-free, so we construct the backend with
// a nullptr api/client probe — mirroring SnapmakerProbe in test_remap_strategy.cpp.

#include "ams_backend_snapmaker.h"

#include "../catch_amalgamated.hpp"

#include <map>
#include <set>
#include <string>

namespace {

// Minimal probe — constructed with nullptr api/client so no Moonraker
// connection is required. build_preprint_gcode is a pure const method that
// never touches api_, so a default/probe instance is sufficient.
class SnapmakerProbe : public AmsBackendSnapmaker {
  public:
    SnapmakerProbe() : AmsBackendSnapmaker(nullptr, nullptr) {}
};

} // namespace

TEST_CASE("Snapmaker build_preprint_gcode used-extruders only (identity map)",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    SECTION("two non-contiguous tools, no remap") {
        REQUIRE(sm.build_preprint_gcode({0, 2}, {}) ==
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2");
    }

    SECTION("all four heads, no remap") {
        REQUIRE(sm.build_preprint_gcode({0, 1, 2, 3}, {}) ==
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,1,2,3");
    }

    SECTION("single tool, no remap") {
        REQUIRE(sm.build_preprint_gcode({1}, {}) == "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1");
    }
}

TEST_CASE("Snapmaker build_preprint_gcode emits extruder map lines for remaps",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    SECTION("single remapped tool") {
        REQUIRE(sm.build_preprint_gcode({1}, {{1, 3}}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=3\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=3");
    }

    SECTION("two remapped tools emit in ascending key order") {
        REQUIRE(sm.build_preprint_gcode({0, 2}, {{0, 1}, {2, 3}}) ==
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
                "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=3\n"
                "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,3");
    }
}

TEST_CASE("Snapmaker build_preprint_gcode dedups colliding heads", "[snapmaker][preprint]") {
    SnapmakerProbe sm;

    // Tool 0 -> head 0 (identity), tool 1 remapped -> head 0. Heads {0,0}
    // collapse to {0}.
    REQUIRE(sm.build_preprint_gcode({0, 1}, {{1, 0}}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=0\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0");
}

TEST_CASE("Snapmaker build_preprint_gcode empty tools_used yields empty string",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    REQUIRE(sm.build_preprint_gcode({}, {}) == "");
}

TEST_CASE("Snapmaker build_preprint_gcode extended tool defaults to head 0",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    // default_head(5) == 0 (firmware default map [0,1,2,3,0,0,...]).
    REQUIRE(sm.build_preprint_gcode({5}, {}) == "SET_PRINT_USED_EXTRUDERS EXTRUDERS=0");
}

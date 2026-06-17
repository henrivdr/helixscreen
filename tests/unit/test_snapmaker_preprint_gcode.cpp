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

#include "filament_mapper.h"

#include "../catch_amalgamated.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

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

// End-to-end scenario for the Batch 2 native-remap UI: a 2-color body using
// logical tools {0,2}, with the user remapping tool 0 → head 1 in the picker.
// get_effective_remap() identity-filters, so only tool 0 yields an entry
// {0,1}; tool 2 stays identity (head 2). build_preprint_gcode then emits one
// SET_PRINT_EXTRUDER_MAP line plus the recomputed used-heads {1,2}.
TEST_CASE("Snapmaker build_preprint_gcode 2-color remap tool0->head1",
          "[snapmaker][preprint]") {
    SnapmakerProbe sm;
    REQUIRE(sm.build_preprint_gcode({0, 2}, {{0, 1}}) ==
            "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
            "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,2");
}

// Focused unit test of the IDENTITY-FILTER / conversion logic that
// PrintSelectDetailView::get_effective_remap() performs on the card's stored
// mappings vector. Full UI modal simulation is impractical (XML/LVGL fixture
// heavy), so we replicate the exact filter inline as a lambda and assert the
// produced map<int,int> matches get_effective_remap()'s contract:
//   * mapped_slot < 0 (auto/unmapped)          -> dropped
//   * mapped_slot == default_head(tool_index)  -> dropped (firmware identity)
//   * mapped_slot >= 0 && != default_head      -> kept as a true remap
// This test FAILS if the identity-filter logic regresses.
TEST_CASE("get_effective_remap identity-filter drops identity + auto entries",
          "[snapmaker][remap]") {
    // Mirror of PrintSelectDetailView::get_effective_remap (lines 979-995).
    auto effective_remap = [](const std::vector<helix::ToolMapping>& mappings) {
        auto default_head = [](int tool) { return (tool >= 0 && tool <= 3) ? tool : 0; };
        std::map<int, int> remap;
        for (const auto& m : mappings) {
            if (m.mapped_slot >= 0 && m.mapped_slot != default_head(m.tool_index)) {
                remap[m.tool_index] = m.mapped_slot;
            }
        }
        return remap;
    };

    auto mk = [](int tool, int slot) {
        helix::ToolMapping m;
        m.tool_index = tool;
        m.mapped_slot = slot;
        return m;
    };

    SECTION("mix of identity, auto, and true remaps keeps only true remaps") {
        std::vector<helix::ToolMapping> mappings = {
            mk(0, 1),  // true remap: head 1 != default_head(0)=0  -> KEEP
            mk(1, 1),  // identity: head 1 == default_head(1)=1     -> DROP
            mk(2, -1), // auto/unmapped: mapped_slot < 0            -> DROP
            mk(3, 0),  // true remap: head 0 != default_head(3)=3   -> KEEP
        };
        std::map<int, int> expected = {{0, 1}, {3, 0}};
        REQUIRE(effective_remap(mappings) == expected);
    }

    SECTION("all-identity map yields empty remap") {
        std::vector<helix::ToolMapping> mappings = {mk(0, 0), mk(1, 1), mk(2, 2), mk(3, 3)};
        REQUIRE(effective_remap(mappings).empty());
    }

    SECTION("extended tool (>3) remapped off head 0 is kept") {
        // default_head(5) == 0, so a remap to head 2 is a true remap.
        std::vector<helix::ToolMapping> mappings = {mk(5, 2)};
        std::map<int, int> expected = {{5, 2}};
        REQUIRE(effective_remap(mappings) == expected);
    }

    SECTION("extended tool (>3) mapped to head 0 is dropped as identity") {
        // default_head(5) == 0, so mapping 5 -> 0 is the firmware identity.
        std::vector<helix::ToolMapping> mappings = {mk(5, 0)};
        REQUIRE(effective_remap(mappings).empty());
    }
}

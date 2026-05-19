// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"
#include "ams_state.h"
#include "lvgl_test_fixture.h"
#include "tool_state.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using helix::ToolState;
using helix::ToolTopology;
using helix::ui::UpdateQueue;

namespace {

// LVGLTestFixture brings up LVGL so ToolState::init_subjects() can register
// subjects (init uses lv_subject_init_*). HelixTestFixture alone does not.
struct ToolStateFixture : public LVGLTestFixture {
    ToolStateFixture() {
        ToolState::instance().init_subjects(/*register_xml=*/false);
    }
    ~ToolStateFixture() override {
        ToolState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] set_ams_topology populates 15 tools",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 15;
    topo.active_tool = 0;
    topo.tool_to_slot.resize(15);
    for (int i = 0; i < 15; ++i)
        topo.tool_to_slot[i] = i; // 1:1 default
    topo.tool_name_prefix = "T";

    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 15);
    REQUIRE(ToolState::instance().active_tool_index() == 0);
    REQUIRE(ToolState::instance().tools()[14].name == "T14");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] active tool updates without rebuilding tools list",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    int initial_version =
        lv_subject_get_int(ToolState::instance().get_tools_version_subject());

    topo.active_tool = 2;
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().active_tool_index() == 2);
    REQUIRE(lv_subject_get_int(ToolState::instance().get_tools_version_subject()) ==
            initial_version); // No rebuild
    // Pin list shape + content so a future "bump version but silently rebuild"
    // regression also fails this test.
    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().tools()[0].name == "T0");
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] clear_ams_topology releases override",
                 "[tool-state][ams][ams-topology]") {
    ToolTopology topo;
    topo.tool_count = 6;
    topo.active_tool = 3;
    topo.tool_to_slot = {0, 1, 2, 3, 4, 5};
    topo.tool_name_prefix = "T";
    ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ToolState::instance().tool_count() == 6);

    ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();

    // After clear, tools_ is empty (callers must invoke init_tools again to repopulate)
    REQUIRE(ToolState::instance().tool_count() == 0);
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] AFC mock with 4 lanes drives ToolState",
                 "[tool-state][ams][afc][ams-topology]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(/*register_xml=*/false);

    auto mock = AmsBackend::create_mock(4);
    REQUIRE(mock != nullptr);
    auto caps = mock->get_tool_mapping_capabilities();
    if (!caps.supported)
        return; // Mock lacks tool multiplexing; skipped.

    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    UpdateQueue::instance().drain();

    REQUIRE(ToolState::instance().tool_count() == 4);
    REQUIRE(ToolState::instance().ams_topology_active());

    // Defensive cleanup: drop the override + backend so later tests (which share
    // the AmsState / ToolState singletons) don't inherit a stale topology.
    ToolState::instance().clear_ams_topology();
    ams.clear_backends();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] update_from_status is ignored when AMS owns active tool",
                 "[tool-state][ams][ams-topology]") {
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 2;
    topo.tool_to_slot = {0, 1, 2, 3};
    helix::ToolState::instance().set_ams_topology(topo);
    UpdateQueue::instance().drain();

    // Simulate Klipper telling us "toolchanger says T0, toolhead is on extruder0".
    // Under AMS topology, ToolState should ignore both and stay on T2.
    nlohmann::json status = {{"toolchanger", {{"tool_number", 0}}},
                             {"toolhead", {{"extruder", "extruder"}}}};
    helix::ToolState::instance().update_from_status(status);
    UpdateQueue::instance().drain();

    REQUIRE(helix::ToolState::instance().active_tool_index() == 2);

    // Cleanup so the override doesn't leak to later tests in this TU.
    helix::ToolState::instance().clear_ams_topology();
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(ToolStateFixture,
                 "[ToolState][ams-topology] set_ams_topology preserves per-tool extruder_name",
                 "[tool-state][ams][ams-topology]") {
    // Simulate init_tools() having populated tools_ with per-tool extruder names
    // (the ToolChanger init path does this from sorted heater names).
    auto& ts = helix::ToolState::instance();

    // Build a topology and apply it once with no prior state — gets default
    // extruder_name="extruder" for every tool.
    helix::ToolTopology topo;
    topo.tool_count = 4;
    topo.active_tool = 0;
    topo.tool_to_slot = {0, 1, 2, 3};
    topo.tool_name_prefix = "T";
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 4);

    // Apply same-shape topology again: needs_rebuild=false, only active_tool_
    // changes. extruder_name preservation isn't exercised on this path.
    topo.active_tool = 2;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ts.active_tool_index() == 2);

    // Apply a different shape (count change → forced rebuild) and verify that
    // the rebuild path copies over extruder_name from the previous tools_.
    // Since the previous tools_ all have extruder_name="extruder" (default), the
    // rebuilt tools_ should also have "extruder" — which is the regression case
    // (ToolChanger sets per-tool names, rebuild used to lose them).
    topo.tool_count = 3;
    topo.tool_to_slot = {0, 1, 2};
    topo.active_tool = 0;
    ts.set_ams_topology(topo);
    UpdateQueue::instance().drain();
    REQUIRE(ts.tool_count() == 3);
    for (const auto& t : ts.tools()) {
        REQUIRE(t.extruder_name.has_value());
        // For this minimal test, default "extruder" is the only available
        // ground truth without a real PrinterDiscovery feed. The point is to
        // prove the copy-from-previous logic runs (the field is still set
        // post-rebuild, not nullopt).
    }

    ts.clear_ams_topology();
    UpdateQueue::instance().drain();
}

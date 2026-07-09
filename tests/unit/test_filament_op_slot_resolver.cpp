// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_op_slot_resolver.cpp
 * @brief Tests for resolve_op_button_slot() — which slot's load-state gates the
 *        FilamentPanel Load/Unload/Purge buttons for the selected tool.
 *
 * Run with: ./build/bin/helix-tests "[filament][op_slot]"
 *
 * Regression guard for prestonbrown/helixscreen#1065: on a single-extruder
 * multi-lane AMS (AD5X native ZMOD IFS) the backend never populates
 * tool_to_slot_map, so the old fallback collapsed the slot to the tool index
 * (0) and read slot 0's load-state — wrongly greying Unload while a *different*
 * lane was loaded to the toolhead. The loaded lane must come from current_slot.
 */

#include "../catch_amalgamated.hpp"

#include "filament_op_slot_resolver.h"

using helix::ui::resolve_op_button_slot;

namespace {

// Minimal AmsSystemInfo tailored for a resolution case.
AmsSystemInfo make_sys(std::vector<int> tool_map, int current_slot) {
    AmsSystemInfo sys;
    sys.tool_to_slot_map = std::move(tool_map);
    sys.current_slot = current_slot;
    return sys;
}

} // namespace

TEST_CASE("AD5X IFS: single tool, no map, loaded lane 1 → slot 1 (Unload enabled)",
          "[filament][op_slot]") {
    // Native ZMOD: tool_to_slot_map is empty, tool 0 fed by lane 1 (current_slot=1).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/1);
    // The bug returned 0 here (tool index) → slot 0 not loaded → Unload greyed.
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 1);
}

TEST_CASE("AD5X IFS: single tool, all-unmapped map (-1 fill) still uses current_slot",
          "[filament][op_slot]") {
    // Some backends size the vector but leave entries at -1 (unmapped).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{-1, -1, -1, -1}, /*current_slot=*/2);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 2);
}

TEST_CASE("Single tool, nothing loaded → -1 (Unload stays disabled)", "[filament][op_slot]") {
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/-1);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == -1);
}

TEST_CASE("Toolchanger: multi-tool, no map → tool index == slot index", "[filament][op_slot]") {
    // A real toolchanger must keep per-tool selection: selecting tool 2 gates on
    // slot 2, NOT on current_slot (which is the *active* tool's lane).
    AmsSystemInfo sys = make_sys(/*tool_map=*/{}, /*current_slot=*/0);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/2, /*tool_count=*/4) == 2);
}

TEST_CASE("Explicit tool→slot map wins over both fallbacks", "[filament][op_slot]") {
    AmsSystemInfo sys = make_sys(/*tool_map=*/{3, 1, 0}, /*current_slot=*/1);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 3);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/2, /*tool_count=*/4) == 0);
}

TEST_CASE("Mapped entry of -1 falls through to topology fallback", "[filament][op_slot]") {
    // tool 1 explicitly unmapped (-1) on a single-tool system → current_slot.
    AmsSystemInfo sys = make_sys(/*tool_map=*/{-1}, /*current_slot=*/3);
    CHECK(resolve_op_button_slot(sys, /*selected_tool=*/0, /*tool_count=*/1) == 3);
}

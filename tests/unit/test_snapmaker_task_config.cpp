// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "snapmaker_task_config.h"
#include "hv/json.hpp"

TEST_CASE("parse print_task_config into AvailableSlots", "[snapmaker][taskcfg]") {
    auto j = nlohmann::json::parse(R"({
        "filament_exist":      [true, false, true, true],
        "filament_color_rgba": ["E72F1DFF","00000000","00C1AEFF","F4C032FF"],
        "filament_type":       ["PLA","PLA","PLA","PETG"]
    })");
    auto slots = helix::parse_snapmaker_task_config(j);
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].slot_index == 0);
    CHECK(slots[0].color_rgb == 0xE72F1D);   // alpha stripped
    CHECK(slots[0].material == "PLA");
    CHECK_FALSE(slots[0].is_empty);
    CHECK(slots[1].is_empty);                // filament_exist[1]==false
    CHECK(slots[3].material == "PETG");
}

TEST_CASE("parse print_task_config tolerates non-object / missing arrays", "[snapmaker][taskcfg]") {
    CHECK(helix::parse_snapmaker_task_config(nlohmann::json()).empty());            // null
    CHECK(helix::parse_snapmaker_task_config(nlohmann::json::parse("{}")).empty()); // no arrays
}

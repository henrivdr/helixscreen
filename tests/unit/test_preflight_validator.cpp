// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "preflight_validator.h"
#include "filament_mapper.h"

using helix::AvailableSlot;
using helix::GcodeToolInfo;
using helix::ToolMapping;
using helix::PreflightValidator;
using Severity = helix::ToolCheck::Severity;

TEST_CASE("empty required slot is flagged and blocks", "[preflight_validator]") {
    std::vector<GcodeToolInfo> tools = {
        {0, 0xED1C24, "PLA"},
        {2, 0x00C1AE, "PLA"},
    };
    std::vector<AvailableSlot> slots = {
        {0, 0, 0xED1C24, "PLA", false, -1, 0, 0, ""},
        {1, 0, 0x000000, "",    true,  -1, 0, 0, ""},
        {2, 0, 0x00C1AE, "PLA", false, -1, 0, 0, ""},
    };
    std::vector<ToolMapping> mapping = {
        {0, 0, 0, false, false, ToolMapping::MatchReason::AUTO},
        {2, 1, 0, false, false, ToolMapping::MatchReason::AUTO},  // green mapped to EMPTY slot 1
    };
    auto result = PreflightValidator::validate(tools, slots, mapping);
    REQUIRE(result.checks.size() == 2);
    CHECK(result.checks[0].severity == Severity::Ok);
    CHECK(result.checks[1].severity == Severity::EmptySlot);
    CHECK(result.checks[1].tool_index == 2);
    CHECK(result.checks[1].mapped_slot == 1);
    CHECK_FALSE(result.checks[1].slot_present);
    CHECK(result.has_block());
    CHECK_FALSE(result.has_advisory());
}

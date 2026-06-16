// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "filament_mapper.h"

namespace helix {

struct ToolCheck {
    int tool_index = -1;
    uint32_t intended_color = 0;
    std::string intended_material;
    int mapped_slot = -1;
    bool slot_present = false;
    bool color_ok = true;
    bool material_ok = true;
    enum class Severity { Ok, ColorMismatch, MaterialMismatch, EmptySlot };
    Severity severity = Severity::Ok;
};

struct PreflightResult {
    std::vector<ToolCheck> checks;
    bool has_block() const;     // any EmptySlot
    bool has_advisory() const;  // any MaterialMismatch
};

class PreflightValidator {
public:
    static PreflightResult validate(const std::vector<GcodeToolInfo>& tools,
                                    const std::vector<AvailableSlot>& slots,
                                    const std::vector<ToolMapping>& mapping);
};

}  // namespace helix

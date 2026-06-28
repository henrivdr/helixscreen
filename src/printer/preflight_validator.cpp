// SPDX-License-Identifier: GPL-3.0-or-later
#include "preflight_validator.h"

#include "filament_mapper.h"

#include <algorithm>

namespace helix {

bool PreflightResult::has_block() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::EmptySlot;
    });
}

bool PreflightResult::has_advisory() const {
    return std::any_of(checks.begin(), checks.end(), [](const ToolCheck& c) {
        return c.severity == ToolCheck::Severity::MaterialMismatch;
    });
}

static int slot_for_tool(int tool_index, const std::vector<ToolMapping>& mapping) {
    for (const auto& m : mapping)
        if (m.tool_index == tool_index)
            return m.mapped_slot;
    return -1;
}
static const AvailableSlot* find_slot(int slot_index, const std::vector<AvailableSlot>& slots) {
    for (const auto& s : slots)
        if (s.slot_index == slot_index)
            return &s;
    return nullptr;
}

PreflightResult PreflightValidator::validate(const std::vector<GcodeToolInfo>& tools,
                                             const std::vector<AvailableSlot>& slots,
                                             const std::vector<ToolMapping>& mapping) {
    PreflightResult out;
    for (const auto& t : tools) {
        ToolCheck c;
        c.tool_index = t.tool_index;
        c.intended_color = t.color_rgb;
        c.intended_material = t.material;
        c.mapped_slot = slot_for_tool(t.tool_index, mapping);
        const AvailableSlot* slot = c.mapped_slot >= 0 ? find_slot(c.mapped_slot, slots) : nullptr;
        if (slot == nullptr || slot->is_empty) {
            c.slot_present = false;
            c.severity = ToolCheck::Severity::EmptySlot;
        } else {
            c.slot_present = true;
            c.color_ok = FilamentMapper::colors_match(t.color_rgb, slot->color_rgb);
            c.material_ok = FilamentMapper::materials_match(t.material, slot->material);
            if (!c.material_ok)
                c.severity = ToolCheck::Severity::MaterialMismatch;
            else if (!c.color_ok)
                c.severity = ToolCheck::Severity::ColorMismatch;
            else
                c.severity = ToolCheck::Severity::Ok;
        }
        out.checks.push_back(std::move(c));
    }
    return out;
}

} // namespace helix

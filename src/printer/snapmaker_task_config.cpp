// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_task_config.h"
#include <string>

namespace helix {

static uint32_t rgba_hex_to_rgb(const std::string& s) {
    if (s.size() < 6) return 0;
    return static_cast<uint32_t>(std::stoul(s.substr(0, 6), nullptr, 16));
}

std::vector<AvailableSlot> parse_snapmaker_task_config(const nlohmann::json& tc) {
    std::vector<AvailableSlot> out;
    if (!tc.is_object()) return out;
    auto exist = tc.value("filament_exist",      nlohmann::json::array());
    auto color = tc.value("filament_color_rgba", nlohmann::json::array());
    auto mat   = tc.value("filament_type",       nlohmann::json::array());
    if (!exist.is_array() || exist.empty()) return out;
    const size_t n = exist.size();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        AvailableSlot s{};
        s.slot_index           = static_cast<int>(i);
        s.backend_index        = 0;
        s.is_empty             = !(exist[i].is_boolean() && exist[i].get<bool>());
        s.color_rgb            = (i < color.size() && color[i].is_string())
                                     ? rgba_hex_to_rgb(color[i].get<std::string>()) : 0;
        s.material             = (i < mat.size() && mat[i].is_string())
                                     ? mat[i].get<std::string>() : "";
        s.current_tool_mapping = -1;
        s.unit_index           = 0;
        s.local_slot_index     = static_cast<int>(i);
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace helix

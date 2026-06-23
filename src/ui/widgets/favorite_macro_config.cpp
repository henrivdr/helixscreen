// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

#include <cstdint>

namespace helix {

FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j) {
    FavoriteMacroConfig c;
    if (j.contains("macro") && j["macro"].is_string())
        c.macro = j["macro"].get<std::string>();
    if (j.contains("icon") && j["icon"].is_string())
        c.icon = j["icon"].get<std::string>();
    if (j.contains("color") && j["color"].is_number_integer()) {
        int64_t v = j["color"].get<int64_t>();
        if (v >= 0 && v <= 0xFFFFFF)
            c.color = static_cast<uint32_t>(v);
    }
    if (j.contains("skip_param_prompt") && j["skip_param_prompt"].is_boolean())
        c.skip_param_prompt = j["skip_param_prompt"].get<bool>();
    return c;
}

nlohmann::json favorite_macro_config_to_json(const FavoriteMacroConfig& c) {
    nlohmann::json j;
    j["macro"] = c.macro;
    if (!c.icon.empty())
        j["icon"] = c.icon;
    if (c.color != 0)
        j["color"] = c.color;
    if (c.skip_param_prompt)
        j["skip_param_prompt"] = true;
    return j;
}

} // namespace helix

// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

namespace helix {

FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j) {
    FavoriteMacroConfig c;
    if (j.contains("macro") && j["macro"].is_string())
        c.macro = j["macro"].get<std::string>();
    if (j.contains("icon") && j["icon"].is_string())
        c.icon = j["icon"].get<std::string>();
    if (j.contains("color") && j["color"].is_number_unsigned())
        c.color = j["color"].get<uint32_t>();
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

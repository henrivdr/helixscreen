// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "hv/json.hpp"

namespace helix {

/// Persisted per-instance config for a FavoriteMacroWidget.
struct FavoriteMacroConfig {
    std::string macro;              ///< Assigned macro name (empty = unconfigured)
    std::string icon;               ///< Custom icon name (empty = "play" default)
    uint32_t color = 0;             ///< Custom icon color RGB (0 = theme default)
    bool skip_param_prompt = false; ///< Run directly, never show the param popup
};

/// Parse config from PanelWidgetConfig JSON (tolerant of missing/wrong types).
FavoriteMacroConfig favorite_macro_config_from_json(const nlohmann::json& j);

/// Serialize config, omitting default-valued fields to keep JSON minimal.
nlohmann::json favorite_macro_config_to_json(const FavoriteMacroConfig& c);

} // namespace helix

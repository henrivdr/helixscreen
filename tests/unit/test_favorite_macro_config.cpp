// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config.h"

#include "../catch_amalgamated.hpp"

using helix::favorite_macro_config_from_json;
using helix::favorite_macro_config_to_json;
using helix::FavoriteMacroConfig;

TEST_CASE("favorite macro config round-trips all fields", "[macro][favorite_config]") {
    FavoriteMacroConfig c;
    c.macro = "BED_MESH_CALIBRATE";
    c.icon = "wrench";
    c.color = 0x7B1FA2;
    c.skip_param_prompt = true;

    auto j = favorite_macro_config_to_json(c);
    auto back = favorite_macro_config_from_json(j);

    REQUIRE(back.macro == "BED_MESH_CALIBRATE");
    REQUIRE(back.icon == "wrench");
    REQUIRE(back.color == 0x7B1FA2u);
    REQUIRE(back.skip_param_prompt == true);
}

TEST_CASE("favorite macro config omits defaults from json", "[macro][favorite_config]") {
    FavoriteMacroConfig c;
    c.macro = "G28";
    // icon empty, color 0, skip false -> omitted

    auto j = favorite_macro_config_to_json(c);
    REQUIRE(j.contains("macro"));
    REQUIRE_FALSE(j.contains("icon"));
    REQUIRE_FALSE(j.contains("color"));
    REQUIRE_FALSE(j.contains("skip_param_prompt"));
}

TEST_CASE("favorite macro config tolerates wrong json types", "[macro][favorite_config]") {
    nlohmann::json j;
    j["macro"] = 42;                // wrong type
    j["skip_param_prompt"] = "yes"; // wrong type
    auto c = favorite_macro_config_from_json(j);
    REQUIRE(c.macro.empty());
    REQUIRE(c.skip_param_prompt == false);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_progress_arc.h"

#include "catch_amalgamated.hpp"

// progress_arc_thickness_tier_for is a pure function — no LVGL fixture needed.

TEST_CASE("progress_arc_thickness_tier_for: tier boundaries", "[progress_arc]") {
    // Tier 0: dim < 80   (4px stroke)
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(0) == 0);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(40) == 0);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(79) == 0);

    // Tier 1: 80..119    (6px)
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(80) == 1);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(100) == 1);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(119) == 1);

    // Tier 2: 120..179   (8px)
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(120) == 2);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(150) == 2);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(179) == 2);

    // Tier 3: 180..239   (10px)
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(180) == 3);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(200) == 3);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(239) == 3);

    // Tier 4: dim >= 240 (12px) — caps here
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(240) == 4);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(500) == 4);
    REQUIRE(helix::ui::progress_arc_thickness_tier_for(10000) == 4);
}

TEST_CASE("progress_arc_thickness_tier_for: monotonic non-decreasing", "[progress_arc]") {
    // As diameter grows, tier should never go backward.
    int prev = -1;
    for (int d = 0; d <= 400; d += 5) {
        int tier = helix::ui::progress_arc_thickness_tier_for(d);
        REQUIRE(tier >= prev);
        REQUIRE(tier >= 0);
        REQUIRE(tier <= 4);
        prev = tier;
    }
}

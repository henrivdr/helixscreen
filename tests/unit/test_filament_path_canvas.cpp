// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_path_canvas.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// hub_box_hit() — pure coordinate hit-test for the selector/hub box.
//
// Box under test: center (200, 90), width 100, height 30, margin 4.
//   X half-extent + margin = 50 + 4 = 54  -> hit when |x - 200| <= 54
//   Y half-extent + margin = 15 + 4 = 19  -> hit when |y - 90|  <= 19
// ============================================================================

namespace {
constexpr int32_t CX = 200;
constexpr int32_t CY = 90;
constexpr int32_t W = 100;
constexpr int32_t H = 30;
constexpr int32_t MARGIN = 4;

bool hit(int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return helix::ui::hub_box_hit(p, CX, CY, W, H, MARGIN);
}
} // namespace

TEST_CASE("hub_box_hit: dead-center point hits", "[canvas][hit_test]") {
    REQUIRE(hit(200, 90));
}

TEST_CASE("hub_box_hit: inside-edge points hit", "[canvas][hit_test]") {
    // Just inside the un-margined box bounds.
    REQUIRE(hit(151, 76));  // left/top inner corner-ish (|x-200|=49, |y-90|=14)
    REQUIRE(hit(249, 104)); // right/bottom (|x-200|=49, |y-90|=14)
}

TEST_CASE("hub_box_hit: exact box-edge points hit", "[canvas][hit_test]") {
    REQUIRE(hit(150, 90));  // left box edge (|x-200|=50 <= 54)
    REQUIRE(hit(250, 90));  // right box edge
    REQUIRE(hit(200, 75));  // top box edge (|y-90|=15 <= 19)
    REQUIRE(hit(200, 105)); // bottom box edge
}

TEST_CASE("hub_box_hit: within-margin points hit", "[canvas][hit_test]") {
    REQUIRE(hit(253, 90));  // |x-200|=53 <= 54
    REQUIRE(hit(254, 90));  // |x-200|=54 == limit, inclusive
    REQUIRE(hit(200, 109)); // |y-90|=19 == limit, inclusive
}

TEST_CASE("hub_box_hit: just-outside-margin points miss", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(255, 90));  // |x-200|=55 > 54
    REQUIRE_FALSE(hit(200, 110)); // |y-90|=20 > 19
}

TEST_CASE("hub_box_hit: far-left point misses", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(140, 90)); // |x-200|=60 > 54
}

TEST_CASE("hub_box_hit: above-box point misses", "[canvas][hit_test]") {
    REQUIRE_FALSE(hit(200, 60)); // |y-90|=30 > 19
}

TEST_CASE("hub_box_hit: argument order is width-then-height", "[canvas][hit_test]") {
    // A wide-short box: 120 wide, 20 tall, margin 0.
    // X half-extent 60, Y half-extent 10. A point at x-offset 55, y-offset 12
    // hits only if width is used for X (55<=60) and height for Y... but 12>10
    // so it MUST miss. If args were swapped (h for X, w for Y), it would hit.
    lv_point_t p{CX + 55, CY + 12};
    REQUIRE_FALSE(helix::ui::hub_box_hit(p, CX, CY, 120, 20, 0));
    // Same x-offset, y-offset 8 -> hits (within both extents).
    lv_point_t p2{CX + 55, CY + 8};
    REQUIRE(helix::ui::hub_box_hit(p2, CX, CY, 120, 20, 0));
}

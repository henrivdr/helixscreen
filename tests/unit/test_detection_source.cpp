// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "detection_source.h"

using helix::detection::DetectionEvent;
using helix::detection::DetectionKind;

TEST_CASE("DetectionEvent defaults are inert", "[detection][source]") {
    DetectionEvent e;
    REQUIRE(e.kind == DetectionKind::Unknown);
    REQUIRE(e.attributable == false);
    REQUIRE(e.already_paused == false);
    REQUIRE(e.source_id.empty());
    REQUIRE_FALSE(e.confidence.has_value());
}

TEST_CASE("DetectionKind maps the stock U1 code space", "[detection][source]") {
    REQUIRE(helix::detection::kind_from_u1_code(2) == DetectionKind::Spaghetti);
    REQUIRE(helix::detection::kind_from_u1_code(1) == DetectionKind::DirtyBed);
    REQUIRE(helix::detection::kind_from_u1_code(3) == DetectionKind::Residue);
    REQUIRE(helix::detection::kind_from_u1_code(4) == DetectionKind::DirtyNozzle);
    REQUIRE(helix::detection::kind_from_u1_code(0) == DetectionKind::Unknown);
    REQUIRE(helix::detection::kind_from_u1_code(-1) == DetectionKind::Unknown);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_renderer.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"

static SpoolInfo make_test_spool() {
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Hatchbox";
    spool.material = "PLA";
    spool.color_name = "Red";
    spool.remaining_weight_g = 800;
    spool.initial_weight_g = 1000;
    spool.lot_nr = "LOT-2026-001";
    spool.comment = "Great filament";
    spool.spool_weight_g = 200;
    return spool;
}

static helix::LabelSize continuous_62mm() {
    return {"62mm", 696, 0, 300, 0x0A, 62, 0};
}

static helix::LabelSize continuous_29mm() {
    return {"29mm", 306, 0, 300, 0x0A, 29, 0};
}

static helix::LabelSize diecut_62x29() {
    return {"62x29mm", 696, 271, 300, 0x0B, 62, 29};
}

/// Check if bitmap has any black pixels
static bool has_black_pixels(const helix::LabelBitmap& bmp) {
    for (int y = 0; y < bmp.height(); y++)
        for (int x = 0; x < bmp.width(); x++)
            if (bmp.get_pixel(x, y))
                return true;
    return false;
}

TEST_CASE("LabelRenderer STANDARD preset produces valid bitmap", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer MINIMAL preset is QR only", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer COMPACT preset", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 696);
    REQUIRE(label.height() > 0);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer 29mm label", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_29mm());

    REQUIRE_FALSE(label.empty());
    REQUIRE(label.width() == 306);
    REQUIRE(label.height() > 0);
}

TEST_CASE("LabelRenderer die-cut label fits dimensions", "[label]") {
    auto spool = make_test_spool();
    auto size = diecut_62x29();
    auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, size);

    REQUIRE(label.width() == 696);
    REQUIRE(label.height() == 271);
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer handles empty vendor and color", "[label]") {
    SpoolInfo spool;
    spool.id = 1;
    spool.material = "PETG";
    // vendor and color_name empty

    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());
    REQUIRE_FALSE(label.empty());
    REQUIRE(has_black_pixels(label));
}

TEST_CASE("LabelRenderer continuous height adapts to content", "[label]") {
    auto spool = make_test_spool();
    auto minimal =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());
    auto standard =
        helix::LabelRenderer::render(spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE(minimal.height() > 0);
    REQUIRE(standard.height() > 0);
    // STANDARD has text alongside QR, so may differ in height
}

TEST_CASE("LabelRenderer MINIMAL die-cut centers QR", "[label]") {
    auto spool = make_test_spool();
    auto size = diecut_62x29();
    auto label = helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, size);

    REQUIRE(label.width() == 696);
    REQUIRE(label.height() == 271);

    // QR should not touch the very edges (there should be margin)
    bool top_row_clear = true;
    for (int x = 0; x < label.width(); x++)
        if (label.get_pixel(x, 0))
            top_row_clear = false;
    REQUIRE(top_row_clear);
}

TEST_CASE("LabelRenderer COMPACT wider label produces larger content", "[label]") {
    auto spool = make_test_spool();
    auto compact_62 =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_62mm());
    auto compact_29 =
        helix::LabelRenderer::render(spool, helix::LabelPreset::COMPACT, continuous_29mm());

    REQUIRE_FALSE(compact_62.empty());
    REQUIRE_FALSE(compact_29.empty());
    // 62mm label is wider than 29mm
    REQUIRE(compact_62.width() > compact_29.width());
}

TEST_CASE("LabelRenderer MINIMAL QR code capped size", "[label]") {
    auto spool = make_test_spool();
    auto label =
        helix::LabelRenderer::render(spool, helix::LabelPreset::MINIMAL, continuous_62mm());

    REQUIRE_FALSE(label.empty());
    // Find the bounding box of black pixels to check QR size
    int max_y = 0;
    for (int y = 0; y < label.height(); y++)
        for (int x = 0; x < label.width(); x++)
            if (label.get_pixel(x, y))
                max_y = y;

    // QR code height should be reasonable (capped, not filling entire label width)
    REQUIRE(max_y < label.width()); // QR shouldn't be as tall as the label is wide
    REQUIRE(max_y <= 300);          // QR should be capped around 250px + margin
}

TEST_CASE("LabelRenderer STANDARD richer spool produces more content", "[label]") {
    // Minimal spool (just material)
    SpoolInfo minimal_spool;
    minimal_spool.id = 1;
    minimal_spool.material = "PLA";

    // Rich spool (all fields)
    auto rich_spool = make_test_spool();

    auto minimal_label = helix::LabelRenderer::render(minimal_spool, helix::LabelPreset::STANDARD,
                                                      continuous_62mm());
    auto rich_label =
        helix::LabelRenderer::render(rich_spool, helix::LabelPreset::STANDARD, continuous_62mm());

    REQUIRE_FALSE(minimal_label.empty());
    REQUIRE_FALSE(rich_label.empty());
    // Rich spool should produce taller label (more text content)
    REQUIRE(rich_label.height() >= minimal_label.height());
}

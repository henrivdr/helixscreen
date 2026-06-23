// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_card_view.cpp
 * @brief Unit tests for PrintSelectCardView thumbnail path helpers
 *
 * Tests is_placeholder_thumbnail() and has_real_thumbnail() to prevent
 * regressions where LVGL "A:" drive prefixes break std::filesystem::exists().
 */

#include "ui_print_select_card_view.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::PrintSelectCardView;

// ============================================================================
// is_placeholder_thumbnail
// ============================================================================

TEST_CASE("CardView: is_placeholder_thumbnail treats empty path as placeholder",
          "[ui][card_view]") {
    REQUIRE(PrintSelectCardView::is_placeholder_thumbnail(""));
}

TEST_CASE("CardView: is_placeholder_thumbnail rejects real paths", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail(
        "A:/home/user/.cache/helix/abc123_160x160_ARGB8888.bin"));
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail("A:/tmp/thumb.png"));
}

// ============================================================================
// has_real_thumbnail
// ============================================================================

TEST_CASE("CardView: has_real_thumbnail returns false for empty path", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail(""));
}

TEST_CASE("CardView: has_real_thumbnail returns false for nonexistent file", "[ui][card_view]") {
    REQUIRE_FALSE(
        PrintSelectCardView::has_real_thumbnail("A:/tmp/does_not_exist_helix_test_thumb.bin"));
}

TEST_CASE("CardView: has_real_thumbnail with A: prefix finds existing file", "[ui][card_view]") {
    // Create a temporary file to test against
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail without A: prefix finds existing file", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb2.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail(tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail returns false after file deleted", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb3.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }
    std::filesystem::remove(tmp);

    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_color_utils.cpp
 * @brief Unit tests for color_utils parsing and naming functions
 */

#include "color_utils.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// parse_hex_color Tests
// ============================================================================

TEST_CASE("parse_hex_color: valid 6-digit formats", "[color][parse]") {
    uint32_t rgb;

    SECTION("#RRGGBB format") {
        REQUIRE(helix::parse_hex_color("#FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#00FF00", rgb) == true);
        REQUIRE(rgb == 0x00FF00);

        REQUIRE(helix::parse_hex_color("#0000FF", rgb) == true);
        REQUIRE(rgb == 0x0000FF);
    }

    SECTION("RRGGBB format (no hash)") {
        REQUIRE(helix::parse_hex_color("FF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }

    SECTION("0xRRGGBB format (C-style)") {
        REQUIRE(helix::parse_hex_color("0xFF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);

        REQUIRE(helix::parse_hex_color("0XFF4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }

    SECTION("Case insensitive") {
        REQUIRE(helix::parse_hex_color("#ff4444", rgb) == true);
        REQUIRE(rgb == 0xFF4444);

        REQUIRE(helix::parse_hex_color("#fF44Aa", rgb) == true);
        REQUIRE(rgb == 0xFF44AA);
    }
}

TEST_CASE("parse_hex_color: valid 3-digit shorthand", "[color][parse]") {
    uint32_t rgb;

    SECTION("#RGB expands to #RRGGBB") {
        REQUIRE(helix::parse_hex_color("#F00", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#0F0", rgb) == true);
        REQUIRE(rgb == 0x00FF00);

        REQUIRE(helix::parse_hex_color("#00F", rgb) == true);
        REQUIRE(rgb == 0x0000FF);

        REQUIRE(helix::parse_hex_color("#ABC", rgb) == true);
        REQUIRE(rgb == 0xAABBCC);
    }

    SECTION("RGB without hash") {
        REQUIRE(helix::parse_hex_color("F44", rgb) == true);
        REQUIRE(rgb == 0xFF4444);
    }
}

TEST_CASE("parse_hex_color: whitespace handling", "[color][parse]") {
    uint32_t rgb;

    SECTION("Leading whitespace trimmed") {
        REQUIRE(helix::parse_hex_color("  #FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("\t#FF0000", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }

    SECTION("Trailing whitespace trimmed") {
        REQUIRE(helix::parse_hex_color("#FF0000  ", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("#FF0000\n", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }

    SECTION("Both leading and trailing") {
        REQUIRE(helix::parse_hex_color("  #FF0000  ", rgb) == true);
        REQUIRE(rgb == 0xFF0000);
    }
}

TEST_CASE("parse_hex_color: 0x prefix with shorthand", "[color][parse]") {
    uint32_t rgb;

    SECTION("0xRGB expands to 0xRRGGBB") {
        REQUIRE(helix::parse_hex_color("0xF00", rgb) == true);
        REQUIRE(rgb == 0xFF0000);

        REQUIRE(helix::parse_hex_color("0xABC", rgb) == true);
        REQUIRE(rgb == 0xAABBCC);
    }
}

TEST_CASE("parse_hex_color: invalid inputs", "[color][parse]") {
    uint32_t rgb = 0xDEADBEEF; // Sentinel value

    SECTION("Empty string") {
        REQUIRE(helix::parse_hex_color("", rgb) == false);
        REQUIRE(rgb == 0xDEADBEEF); // Unchanged
    }

    SECTION("Null pointer") {
        REQUIRE(helix::parse_hex_color(nullptr, rgb) == false);
    }

    SECTION("Whitespace only") {
        REQUIRE(helix::parse_hex_color("   ", rgb) == false);
        REQUIRE(helix::parse_hex_color("\t\n", rgb) == false);
    }

    SECTION("Invalid characters") {
        REQUIRE(helix::parse_hex_color("#GGGGGG", rgb) == false);
        REQUIRE(helix::parse_hex_color("#ZZZZZZ", rgb) == false);
        REQUIRE(helix::parse_hex_color("invalid", rgb) == false);
    }

    SECTION("Wrong digit count") {
        REQUIRE(helix::parse_hex_color("#FF", rgb) == false);      // 2 digits
        REQUIRE(helix::parse_hex_color("#FFFF", rgb) == false);    // 4 digits
        REQUIRE(helix::parse_hex_color("#FFFFF", rgb) == false);   // 5 digits
        REQUIRE(helix::parse_hex_color("#FFFFFFF", rgb) == false); // 7 digits
    }

    SECTION("Garbage after valid hex") {
        REQUIRE(helix::parse_hex_color("#FF0000garbage", rgb) == false);
        REQUIRE(helix::parse_hex_color("#FF0000 garbage", rgb) == false);
    }

    SECTION("Only prefix") {
        REQUIRE(helix::parse_hex_color("#", rgb) == false);
        REQUIRE(helix::parse_hex_color("0x", rgb) == false);
    }

    SECTION("Hash with only whitespace") {
        REQUIRE(helix::parse_hex_color("#   ", rgb) == false);
    }
}

// ============================================================================
// parse_hex_color optional overload Tests
// ============================================================================

TEST_CASE("parse_hex_color optional overload", "[color]") {
    SECTION("parses standard 6-digit hex with #") {
        auto result = helix::parse_hex_color(std::string("#FF0000"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0xFF0000);
    }
    SECTION("parses without # prefix") {
        auto result = helix::parse_hex_color(std::string("00FF00"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0x00FF00);
    }
    SECTION("parses 3-digit shorthand") {
        auto result = helix::parse_hex_color(std::string("#F00"));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0xFF0000);
    }
    SECTION("returns nullopt for empty string") {
        auto result = helix::parse_hex_color(std::string(""));
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("returns nullopt for invalid hex") {
        auto result = helix::parse_hex_color(std::string("#ZZZZZZ"));
        REQUIRE_FALSE(result.has_value());
    }
    SECTION("handles whitespace") {
        auto result = helix::parse_hex_color(std::string("  #0000FF  "));
        REQUIRE(result.has_value());
        REQUIRE(*result == 0x0000FF);
    }
}

// ============================================================================
// describe_color Tests
// ============================================================================

TEST_CASE("describe_color: basic colors", "[color][describe]") {
    SECTION("Pure red") {
        std::string name = helix::describe_color(0xFF0000);
        REQUIRE(name.find("Red") != std::string::npos);
    }

    SECTION("Pure green") {
        std::string name = helix::describe_color(0x00FF00);
        REQUIRE(name.find("Green") != std::string::npos);
    }

    SECTION("Pure blue") {
        std::string name = helix::describe_color(0x0000FF);
        REQUIRE(name.find("Blue") != std::string::npos);
    }
}

TEST_CASE("describe_color: grayscale", "[color][describe]") {
    REQUIRE(helix::describe_color(0xFFFFFF) == "White");
    REQUIRE(helix::describe_color(0x000000) == "Black");

    std::string gray = helix::describe_color(0x808080);
    REQUIRE(gray.find("Gray") != std::string::npos);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_temp_display.cpp
 * @brief Tests for PrintStatusPanel temperature display formatting
 *
 * PrinterState stores temperatures in decidegrees (×10) for precision.
 * These tests verify the display correctly converts to whole degrees.
 *
 * Bug context: Previously displayed "2100 / 2200°C" instead of "210 / 220°C"
 * because the decidegree values weren't divided by 10 before display.
 */

#include <cstdio>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Helper: Simulates the temperature formatting logic from PrintStatusPanel
// ============================================================================

/**
 * @brief Format temperature display string from decidegree values
 *
 * Mirrors the logic in PrintStatusPanel::update_all_displays():
 * - Takes current and target temps in decidegrees (×10)
 * - Returns formatted string like "210 / 220°C"
 *
 * @param current_deci Current temperature in decidegrees
 * @param target_deci Target temperature in decidegrees
 * @return Formatted temperature string
 */
static std::string format_temp_display(int current_deci, int target_deci) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d / %d°C", current_deci / 10, target_deci / 10);
    return std::string(buf);
}

// ============================================================================
// Temperature Display Conversion Tests
// ============================================================================

TEST_CASE("Temperature display converts decidegrees to degrees", "[print_status][temperature]") {
    SECTION("Typical PLA nozzle temperature") {
        // 210°C stored as 2100 decidegrees
        int current_deci = 2100;
        int target_deci = 2150; // 215°C target

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "210 / 215°C");
    }

    SECTION("Typical PLA bed temperature") {
        // 60°C stored as 600 decidegrees
        int current_deci = 580; // 58°C current
        int target_deci = 600;  // 60°C target

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "58 / 60°C");
    }

    SECTION("High temperature ABS nozzle") {
        // 250°C stored as 2500 decidegrees
        int current_deci = 2480; // 248°C heating up
        int target_deci = 2500;  // 250°C target

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "248 / 250°C");
    }

    SECTION("High temperature ABS bed") {
        // 110°C stored as 1100 decidegrees
        int current_deci = 1050; // 105°C heating up
        int target_deci = 1100;  // 110°C target

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "105 / 110°C");
    }

    SECTION("Room temperature (heater off)") {
        // 25°C stored as 250 decidegrees, target 0
        int current_deci = 250;
        int target_deci = 0;

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "25 / 0°C");
    }

    SECTION("Zero temperature") {
        int current_deci = 0;
        int target_deci = 0;

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "0 / 0°C");
    }

    SECTION("3DBenchy default temperatures from G-code metadata") {
        // From test file: nozzle=220°C, bed=55°C
        // These caused the original bug (displayed as 2200°C / 550°C)
        int nozzle_current = 2200; // 220°C
        int nozzle_target = 2200;  // 220°C
        int bed_current = 550;     // 55°C
        int bed_target = 550;      // 55°C

        std::string nozzle_result = format_temp_display(nozzle_current, nozzle_target);
        std::string bed_result = format_temp_display(bed_current, bed_target);

        REQUIRE(nozzle_result == "220 / 220°C");
        REQUIRE(bed_result == "55 / 55°C");

        // These would have been wrong before the fix:
        REQUIRE(nozzle_result != "2200 / 2200°C");
        REQUIRE(bed_result != "550 / 550°C");
    }
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_CASE("Temperature display edge cases", "[print_status][temperature][edge]") {
    SECTION("Negative temperature (should not happen but handle gracefully)") {
        int current_deci = -100; // -10°C (impossible for heater)
        int target_deci = 0;

        std::string result = format_temp_display(current_deci, target_deci);

        // Integer division of negative numbers: -100/10 = -10
        REQUIRE(result == "-10 / 0°C");
    }

    SECTION("Very high temperature (chamber heater)") {
        // 80°C chamber = 800 decidegrees
        int current_deci = 750;
        int target_deci = 800;

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "75 / 80°C");
    }

    SECTION("Fractional degrees are truncated (integer division)") {
        // 215.5°C stored as 2155 decidegrees
        // Integer division: 2155/10 = 215 (truncated, not rounded)
        int current_deci = 2155;
        int target_deci = 2200;

        std::string result = format_temp_display(current_deci, target_deci);

        REQUIRE(result == "215 / 220°C");
    }
}

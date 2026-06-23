// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/ui_temp_graph_scaling.h"

#include "../catch_amalgamated.hpp"

// Default params for most tests (matches mini graph defaults)
static constexpr TempGraphScaleParams P{};

// ============================================================================
// Basic Behavior Tests
// ============================================================================

TEST_CASE("Y-axis scaling returns unchanged value when no scaling needed", "[scaling][basic]") {
    SECTION("Room temperature - stays at floor") {
        float result = calculate_temp_graph_y_max(150.0f, 25.0f, 25.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Mid-range temps in dead zone - stays at current max") {
        // expand threshold = 200*0.80 = 160, shrink = 200*0.60 = 120
        // 130 is between them -> no change
        float result = calculate_temp_graph_y_max(200.0f, 130.0f, 130.0f, P);
        REQUIRE(result == 200.0f);
    }

    SECTION("High temps but below expand threshold - stays at current max") {
        // 80% of 200 = 160, so 155 shouldn't trigger expansion
        float result = calculate_temp_graph_y_max(200.0f, 155.0f, 155.0f, P);
        REQUIRE(result == 200.0f);
    }
}

// ============================================================================
// Expansion Tests
// ============================================================================

TEST_CASE("Y-axis expands when temps approach max", "[scaling][expand]") {
    SECTION("Expand from 150 to 200 at 80% threshold") {
        // 80% of 150 = 120, so 121 should trigger expansion
        float result = calculate_temp_graph_y_max(150.0f, 121.0f, 121.0f, P);
        REQUIRE(result == 200.0f);
    }

    SECTION("Expand from 200 to 250") {
        // 80% of 200 = 160
        float result = calculate_temp_graph_y_max(200.0f, 165.0f, 165.0f, P);
        REQUIRE(result == 250.0f);
    }

    SECTION("Expand from 250 to 300") {
        // 80% of 250 = 200
        float result = calculate_temp_graph_y_max(250.0f, 205.0f, 205.0f, P);
        REQUIRE(result == 300.0f);
    }

    SECTION("Does not expand beyond ceiling") {
        float result = calculate_temp_graph_y_max(300.0f, 280.0f, 280.0f, P);
        REQUIRE(result == 300.0f);
    }
}

// ============================================================================
// Shrink Tests
// ============================================================================

TEST_CASE("Y-axis shrinks when temps drop below threshold", "[scaling][shrink]") {
    SECTION("Shrink from 200 to 150 when temps low") {
        // Shrink: 25 < 200*0.60 = 120 -> yes, shrink to ceil(25/50)*50 = 50, clamped to floor=150
        float result = calculate_temp_graph_y_max(200.0f, 25.0f, 25.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Shrink from 250 to 150 when temps very low") {
        // 50 < 250*0.60 = 150 -> yes, shrink to ceil(50/50)*50 = 100, clamped to floor=150
        float result = calculate_temp_graph_y_max(250.0f, 50.0f, 50.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Shrink from 300 limited by buffer_max") {
        // Current temps low but buffer still has 200 from a spike
        float result = calculate_temp_graph_y_max(300.0f, 50.0f, 250.0f, P);
        REQUIRE(result == 300.0f); // buffer_max=250 -> min_for_data=300
    }

    SECTION("Does not shrink below floor") {
        float result = calculate_temp_graph_y_max(150.0f, 10.0f, 10.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Does not shrink if temps above shrink threshold") {
        // Shrink threshold for 200: 200*0.60 = 120. Temp 125 > 120 -> no shrink
        float result = calculate_temp_graph_y_max(200.0f, 125.0f, 125.0f, P);
        REQUIRE(result == 200.0f);
    }
}

// ============================================================================
// Buffer Max Floor Tests
// ============================================================================

TEST_CASE("Buffer max prevents Y-axis from shrinking below visible data", "[scaling][buffer]") {
    SECTION("Spike in buffer prevents shrink") {
        // Current temp is low, but buffer has a 200 spike
        // Without buffer_max: would shrink to 150
        // With buffer_max=200: min_for_data = ceil(200/50)*50 = 250
        float result = calculate_temp_graph_y_max(300.0f, 25.0f, 200.0f, P);
        REQUIRE(result == 250.0f);
    }

    SECTION("Buffer max near ceiling keeps range high") {
        float result = calculate_temp_graph_y_max(300.0f, 25.0f, 280.0f, P);
        REQUIRE(result == 300.0f);
    }

    SECTION("Zero buffer max has no effect") {
        float result = calculate_temp_graph_y_max(200.0f, 25.0f, 0.0f, P);
        REQUIRE(result == 150.0f); // Normal shrink
    }
}

// ============================================================================
// Hysteresis Tests (prevent oscillation)
// ============================================================================

TEST_CASE("Hysteresis prevents oscillation near thresholds", "[scaling][hysteresis]") {
    SECTION("Dead zone between expand and shrink thresholds") {
        // At max=200: expand = 160, shrink = 120
        // Temps between 120 and 160 should not change anything
        float result = calculate_temp_graph_y_max(200.0f, 130.0f, 130.0f, P);
        REQUIRE(result == 200.0f);

        result = calculate_temp_graph_y_max(200.0f, 155.0f, 155.0f, P);
        REQUIRE(result == 200.0f);
    }

    SECTION("After expansion, won't immediately shrink") {
        // Expand from 150 to 200 at 121
        float after_expand = calculate_temp_graph_y_max(150.0f, 121.0f, 121.0f, P);
        REQUIRE(after_expand == 200.0f);

        // Now at 200, with temp at 121 — NOT below shrink threshold (120)
        float next = calculate_temp_graph_y_max(200.0f, 121.0f, 121.0f, P);
        REQUIRE(next == 200.0f);
    }

    SECTION("After shrink, won't immediately expand") {
        // At 200, shrink to 150 when temps at 25
        float after_shrink = calculate_temp_graph_y_max(200.0f, 25.0f, 25.0f, P);
        REQUIRE(after_shrink == 150.0f);

        // Now at 150, temp rises to 100 — NOT above expand threshold (120)
        float next = calculate_temp_graph_y_max(150.0f, 100.0f, 100.0f, P);
        REQUIRE(next == 150.0f);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("Edge cases for Y-axis scaling", "[scaling][edge]") {
    SECTION("Zero temperatures") {
        float result = calculate_temp_graph_y_max(150.0f, 0.0f, 0.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Negative temperatures (cold environment)") {
        float result = calculate_temp_graph_y_max(150.0f, -10.0f, -10.0f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Exactly at expand threshold - no expand") {
        // 80% of 150 = 120 exactly
        float result = calculate_temp_graph_y_max(150.0f, 120.0f, 120.0f, P);
        REQUIRE(result == 150.0f); // Need > threshold, not >=
    }

    SECTION("Just above expand threshold - expand") {
        float result = calculate_temp_graph_y_max(150.0f, 120.1f, 120.1f, P);
        REQUIRE(result == 200.0f);
    }

    SECTION("Above shrink threshold - no shrink") {
        // 60% of 200 = ~120. Use 121 to be clearly above threshold
        float result = calculate_temp_graph_y_max(200.0f, 121.0f, 121.0f, P);
        REQUIRE(result == 200.0f);
    }

    SECTION("Just below shrink threshold - shrink") {
        float result = calculate_temp_graph_y_max(200.0f, 119.9f, 119.9f, P);
        REQUIRE(result == 150.0f);
    }

    SECTION("Very high temperature capped at ceiling") {
        float result = calculate_temp_graph_y_max(300.0f, 500.0f, 500.0f, P);
        REQUIRE(result == 300.0f);
    }
}

// ============================================================================
// Multi-step Scaling Tests
// ============================================================================

TEST_CASE("Multi-step scaling scenarios", "[scaling][integration]") {
    SECTION("Full heat cycle: room temp -> 300 -> cool down") {
        float y_max = 150.0f;

        // Start at room temp
        y_max = calculate_temp_graph_y_max(y_max, 25.0f, 25.0f, P);
        REQUIRE(y_max == 150.0f);

        // Heat up - expand at 121 (80% of 150 = 120)
        y_max = calculate_temp_graph_y_max(y_max, 125.0f, 125.0f, P);
        REQUIRE(y_max == 200.0f);

        // Continue - expand at 161 (80% of 200 = 160)
        y_max = calculate_temp_graph_y_max(y_max, 165.0f, 165.0f, P);
        REQUIRE(y_max == 250.0f);

        // Continue - expand at 201 (80% of 250 = 200)
        y_max = calculate_temp_graph_y_max(y_max, 205.0f, 205.0f, P);
        REQUIRE(y_max == 300.0f);

        // At max, stabilize
        y_max = calculate_temp_graph_y_max(y_max, 280.0f, 280.0f, P);
        REQUIRE(y_max == 300.0f);

        // Cool down - shrink when below 60% thresholds
        // buffer_max tracks current temps (no historical spike in this test)
        y_max = calculate_temp_graph_y_max(y_max, 50.0f, 50.0f, P);
        REQUIRE(y_max == 150.0f); // Falls all the way to floor

        // Back to room temp - stays at floor
        y_max = calculate_temp_graph_y_max(y_max, 25.0f, 25.0f, P);
        REQUIRE(y_max == 150.0f);
    }
}

// ============================================================================
// Legacy Wrapper Tests
// ============================================================================

TEST_CASE("Legacy mini graph wrapper", "[scaling][legacy]") {
    SECTION("Basic usage without buffer_max") {
        float result = calculate_mini_graph_y_max(150.0f, 25.0f, 25.0f);
        REQUIRE(result == 150.0f);
    }

    SECTION("With buffer_max prevents shrink") {
        // Would normally shrink, but buffer has spike
        float result = calculate_mini_graph_y_max(250.0f, 25.0f, 25.0f, 200.0f);
        REQUIRE(result == 250.0f); // buffer_max=200 -> min_for_data=250
    }
}

// ============================================================================
// Custom Parameters Tests
// ============================================================================

TEST_CASE("Custom scale parameters", "[scaling][params]") {
    SECTION("Widget uses different thresholds") {
        constexpr TempGraphScaleParams widget_p{.step = 50.0f,
                                                .floor = 100.0f,
                                                .ceiling = 400.0f,
                                                .expand_threshold = 0.90f,
                                                .shrink_threshold = 0.50f};

        // More aggressive expand (90% vs 80%)
        float result = calculate_temp_graph_y_max(200.0f, 175.0f, 175.0f, widget_p);
        REQUIRE(result == 200.0f); // 175 < 200*0.90=180, no expand

        result = calculate_temp_graph_y_max(200.0f, 185.0f, 185.0f, widget_p);
        REQUIRE(result == 250.0f); // 185 > 180, expand

        // More conservative shrink (50% vs 60%)
        result = calculate_temp_graph_y_max(200.0f, 105.0f, 105.0f, widget_p);
        REQUIRE(result == 200.0f); // 105 > 200*0.50=100, no shrink
    }
}

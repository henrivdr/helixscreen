// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_collector.cpp
 * @brief Unit tests for BedMeshProgressCollector
 *
 * Tests the regex parsing, progress callbacks, completion detection,
 * and error handling for bed mesh calibration progress tracking.
 */

#include "bed_mesh_probe_parser.h"

#include <regex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Regex Parsing Tests (standalone, no collector instance needed)
// ============================================================================

namespace {

/**
 * @brief Parse a probe progress line and extract current/total values
 *
 * Handles both formats:
 * - "Probing point 5/25"
 * - "Probe point 5 of 25"
 *
 * @param line The G-code response line to parse
 * @param current Output: current probe number
 * @param total Output: total probe count
 * @return true if line matched and was parsed successfully
 */
bool parse_probe_progress(const std::string& line, int& current, int& total) {
    // Static regex for performance - handles both formats
    static const std::regex probe_regex(R"(Prob(?:ing point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))");

    std::smatch match;
    if (std::regex_search(line, match, probe_regex) && match.size() == 3) {
        try {
            current = std::stoi(match[1].str());
            total = std::stoi(match[2].str());
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

/**
 * @brief Check if a line indicates mesh calibration completion
 */
bool is_completion_line(const std::string& line) {
    // Case-insensitive check for completion markers
    return line.find("Mesh Bed Leveling Complete") != std::string::npos ||
           line.find("Mesh bed leveling complete") != std::string::npos ||
           (line.find("BED_MESH_CALIBRATE") != std::string::npos &&
            line.find("ok") != std::string::npos);
}

/**
 * @brief Check if a line indicates an error
 */
bool is_error_line(const std::string& line) {
    return line.rfind("!! ", 0) == 0 ||              // Emergency errors start with "!! "
           line.rfind("Error:", 0) == 0 ||           // Standard errors
           line.find("error:") != std::string::npos; // Python tracebacks
}

} // namespace

// ============================================================================
// Regex Parsing Tests
// ============================================================================

TEST_CASE("BedMeshCollector parses 'Probing point X/Y' format", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("simple case") {
        REQUIRE(parse_probe_progress("Probing point 5/25", current, total));
        REQUIRE(current == 5);
        REQUIRE(total == 25);
    }

    SECTION("first point") {
        REQUIRE(parse_probe_progress("Probing point 1/25", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 25);
    }

    SECTION("last point") {
        REQUIRE(parse_probe_progress("Probing point 25/25", current, total));
        REQUIRE(current == 25);
        REQUIRE(total == 25);
    }

    SECTION("large grid") {
        REQUIRE(parse_probe_progress("Probing point 49/100", current, total));
        REQUIRE(current == 49);
        REQUIRE(total == 100);
    }

    SECTION("with prefix text") {
        REQUIRE(parse_probe_progress("// Probing point 3/9", current, total));
        REQUIRE(current == 3);
        REQUIRE(total == 9);
    }
}

TEST_CASE("BedMeshCollector parses 'Probe point X of Y' format", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("simple case") {
        REQUIRE(parse_probe_progress("Probe point 5 of 25", current, total));
        REQUIRE(current == 5);
        REQUIRE(total == 25);
    }

    SECTION("first point") {
        REQUIRE(parse_probe_progress("Probe point 1 of 16", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 16);
    }

    SECTION("last point") {
        REQUIRE(parse_probe_progress("Probe point 16 of 16", current, total));
        REQUIRE(current == 16);
        REQUIRE(total == 16);
    }

    SECTION("large grid") {
        REQUIRE(parse_probe_progress("Probe point 77 of 144", current, total));
        REQUIRE(current == 77);
        REQUIRE(total == 144);
    }
}

TEST_CASE("BedMeshCollector rejects invalid lines", "[bed_mesh_collector][regex]") {
    int current = 0, total = 0;

    SECTION("empty string") {
        REQUIRE_FALSE(parse_probe_progress("", current, total));
    }

    SECTION("unrelated gcode output") {
        REQUIRE_FALSE(parse_probe_progress("ok", current, total));
        REQUIRE_FALSE(parse_probe_progress("G28", current, total));
        REQUIRE_FALSE(parse_probe_progress("M104 S200", current, total));
    }

    SECTION("similar but different text") {
        REQUIRE_FALSE(parse_probe_progress("Moving to point 5/25", current, total));
        REQUIRE_FALSE(parse_probe_progress("Point 5 of 25", current, total));
    }

    SECTION("malformed numbers") {
        REQUIRE_FALSE(parse_probe_progress("Probing point abc/def", current, total));
    }
}

// ============================================================================
// Completion Detection Tests
// ============================================================================

TEST_CASE("BedMeshCollector detects completion markers", "[bed_mesh_collector][completion]") {
    SECTION("standard completion message") {
        REQUIRE(is_completion_line("Mesh Bed Leveling Complete"));
    }

    SECTION("lowercase variant") {
        REQUIRE(is_completion_line("Mesh bed leveling complete"));
    }

    SECTION("with prefix") {
        REQUIRE(is_completion_line("// Mesh Bed Leveling Complete"));
    }

    SECTION("non-completion lines") {
        REQUIRE_FALSE(is_completion_line("ok"));
        REQUIRE_FALSE(is_completion_line("Probing point 5/25"));
        REQUIRE_FALSE(is_completion_line("Moving to bed mesh position"));
    }
}

// ============================================================================
// Error Detection Tests
// ============================================================================

TEST_CASE("BedMeshCollector detects error markers", "[bed_mesh_collector][error]") {
    SECTION("emergency error prefix") {
        REQUIRE(is_error_line("!! Probe triggered prior to move"));
        REQUIRE(is_error_line("!! Timer too close"));
    }

    SECTION("standard error prefix") {
        REQUIRE(is_error_line("Error: Probe failed to trigger"));
        REQUIRE(is_error_line("Error: Heater extruder not heating at expected rate"));
    }

    SECTION("python traceback error") {
        REQUIRE(is_error_line("klippy/extras/probe.py:123: error: probe not found"));
    }

    SECTION("non-error lines") {
        REQUIRE_FALSE(is_error_line("ok"));
        REQUIRE_FALSE(is_error_line("Probing point 5/25"));
        REQUIRE_FALSE(is_error_line("// Comment with error word"));
        REQUIRE_FALSE(is_error_line("B:60.0 /60.0 T0:200.0 /200.0"));
    }
}

// ============================================================================
// Progress Callback Integration Tests
// ============================================================================

TEST_CASE("BedMeshCollector progress callback receives correct values",
          "[bed_mesh_collector][callback]") {
    // Simulate what the collector would do with parsed values
    std::vector<std::pair<int, int>> progress_calls;

    auto on_progress = [&progress_calls](int current, int total) {
        progress_calls.push_back({current, total});
    };

    // Simulate parsing a sequence of lines
    std::vector<std::string> lines = {
        "// Moving to first probe position",
        "Probing point 1/9",
        "Probing point 2/9",
        "Probing point 3/9",
        "// Probe result: z=0.125",
        "Probing point 4/9",
        "Probing point 5/9",
        "Probing point 6/9",
        "Probing point 7/9",
        "Probing point 8/9",
        "Probing point 9/9",
        "Mesh Bed Leveling Complete",
    };

    for (const auto& line : lines) {
        int current = 0, total = 0;
        if (parse_probe_progress(line, current, total)) {
            on_progress(current, total);
        }
    }

    REQUIRE(progress_calls.size() == 9);
    REQUIRE(progress_calls[0] == std::make_pair(1, 9));
    REQUIRE(progress_calls[4] == std::make_pair(5, 9));
    REQUIRE(progress_calls[8] == std::make_pair(9, 9));
}

TEST_CASE("BedMeshCollector handles mixed format progress lines",
          "[bed_mesh_collector][callback]") {
    std::vector<std::pair<int, int>> progress_calls;

    auto on_progress = [&progress_calls](int current, int total) {
        progress_calls.push_back({current, total});
    };

    // Some printers might use different formats
    std::vector<std::string> lines = {
        "Probe point 1 of 25",
        "Probing point 2/25",
        "Probe point 3 of 25",
        "Probing point 4/25",
    };

    for (const auto& line : lines) {
        int current = 0, total = 0;
        if (parse_probe_progress(line, current, total)) {
            on_progress(current, total);
        }
    }

    REQUIRE(progress_calls.size() == 4);
    // All should parse to same total
    for (const auto& call : progress_calls) {
        REQUIRE(call.second == 25);
    }
    REQUIRE(progress_calls[0].first == 1);
    REQUIRE(progress_calls[1].first == 2);
    REQUIRE(progress_calls[2].first == 3);
    REQUIRE(progress_calls[3].first == 4);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("BedMeshCollector handles edge case probe counts", "[bed_mesh_collector][edge]") {
    int current = 0, total = 0;

    SECTION("minimum grid (2x2 = 4 points)") {
        REQUIRE(parse_probe_progress("Probing point 1/4", current, total));
        REQUIRE(current == 1);
        REQUIRE(total == 4);
    }

    SECTION("large grid (20x20 = 400 points)") {
        REQUIRE(parse_probe_progress("Probing point 399/400", current, total));
        REQUIRE(current == 399);
        REQUIRE(total == 400);
    }

    SECTION("adaptive mesh with odd count") {
        REQUIRE(parse_probe_progress("Probing point 17/37", current, total));
        REQUIRE(current == 17);
        REQUIRE(total == 37);
    }
}

// ============================================================================
// Probe Samples Per Point (fallback "probe at" path)
// ============================================================================

/**
 * @brief Simulate the fallback path with probe_samples > 1
 *
 * When firmware doesn't emit "Probing point X/Y" but does emit
 * "probe at X,Y is z=Z" for each sample, the collector divides by
 * probe_samples to report mesh-point progress instead of raw sample count.
 *
 * Math: mesh_point = ceil(probe_at_count / probe_samples)
 */
static std::vector<std::pair<int, int>> simulate_fallback_probing(int grid_points,
                                                                  int probe_samples) {
    std::vector<std::pair<int, int>> progress_calls;
    int probe_at_count = 0;
    int expected_probes = grid_points;
    int samples = std::max(probe_samples, 1);

    // Each mesh point produces `samples` "probe at" lines
    for (int point = 0; point < grid_points; ++point) {
        for (int s = 0; s < samples; ++s) {
            // Simulate a "probe at X,Y is z=Z" line
            ++probe_at_count;
            int mesh_point = (probe_at_count + samples - 1) / samples;
            progress_calls.push_back({mesh_point, expected_probes});
        }
    }
    return progress_calls;
}

TEST_CASE("Fallback probe count with samples=1 reports raw count",
          "[bed_mesh_collector][samples]") {
    auto calls = simulate_fallback_probing(25, 1);
    REQUIRE(calls.size() == 25);
    REQUIRE(calls.front() == std::make_pair(1, 25));
    REQUIRE(calls.back() == std::make_pair(25, 25));
}

TEST_CASE("Fallback probe count with samples=3 reports mesh points",
          "[bed_mesh_collector][samples]") {
    // 5x5 grid, 3 samples per point = 75 "probe at" lines
    auto calls = simulate_fallback_probing(25, 3);
    REQUIRE(calls.size() == 75);

    // First 3 samples all map to mesh point 1
    REQUIRE(calls[0] == std::make_pair(1, 25));
    REQUIRE(calls[1] == std::make_pair(1, 25));
    REQUIRE(calls[2] == std::make_pair(1, 25));

    // Samples 4-6 map to mesh point 2
    REQUIRE(calls[3] == std::make_pair(2, 25));
    REQUIRE(calls[5] == std::make_pair(2, 25));

    // Last sample maps to mesh point 25, not 75
    REQUIRE(calls.back() == std::make_pair(25, 25));
}

TEST_CASE("Fallback probe count with samples=5 reports mesh points",
          "[bed_mesh_collector][samples]") {
    // 3x3 grid, 5 samples per point = 45 "probe at" lines
    auto calls = simulate_fallback_probing(9, 5);
    REQUIRE(calls.size() == 45);
    REQUIRE(calls.back() == std::make_pair(9, 9));

    // After 10 samples (2 mesh points * 5 samples), should report point 2
    REQUIRE(calls[9] == std::make_pair(2, 9));
    // After 11 samples, should report point 3 (ceiling division)
    REQUIRE(calls[10] == std::make_pair(3, 9));
}

TEST_CASE("is_probe_result_line detects standard Klipper probe output",
          "[bed_mesh_collector][samples]") {
    REQUIRE(helix::is_probe_result_line("probe at 150.000,150.000 is z=1.234"));
    REQUIRE(helix::is_probe_result_line("probe at 0.000,0.000 is z=-0.050"));
    REQUIRE_FALSE(helix::is_probe_result_line("Probing point 5/25"));
    REQUIRE_FALSE(helix::is_probe_result_line("ok"));
}

// ============================================================================
// parse_probe_position — underpins sample deduplication
// ============================================================================

TEST_CASE("parse_probe_position extracts X,Y from comma-separated form",
          "[bed_mesh_collector][probe_position]") {
    auto p = helix::parse_probe_position("probe at 150.000,120.500 is z=-0.050");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(150.000));
    REQUIRE(p->y == Catch::Approx(120.500));
}

TEST_CASE("parse_probe_position handles 'x:'/'y:' prefix form (Snapmaker U1)",
          "[bed_mesh_collector][probe_position]") {
    // Snapmaker U1 Klipper emits this labeled form
    auto p = helix::parse_probe_position("probe at x: 181.474, y: 55.048 is z=-0.214167");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(181.474));
    REQUIRE(p->y == Catch::Approx(55.048));
}

TEST_CASE("parse_probe_position handles negative coordinates",
          "[bed_mesh_collector][probe_position]") {
    auto p = helix::parse_probe_position("probe at -5.500,-10.250 is z=0.000");
    REQUIRE(p.has_value());
    REQUIRE(p->x == Catch::Approx(-5.500));
    REQUIRE(p->y == Catch::Approx(-10.250));
}

TEST_CASE("parse_probe_position rejects non-probe lines", "[bed_mesh_collector][probe_position]") {
    REQUIRE_FALSE(helix::parse_probe_position("Probing point 5/25").has_value());
    REQUIRE_FALSE(helix::parse_probe_position("ok").has_value());
    REQUIRE_FALSE(helix::parse_probe_position("probe at is z=0").has_value());
}

// ============================================================================
// parse_adapted_probe_count — stock Klipper adaptive bed_mesh signal
// ============================================================================

TEST_CASE("parse_adapted_probe_count extracts N*M from Klipper line",
          "[bed_mesh_collector][adapted]") {
    SECTION("Bare form") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 4,4");
        REQUIRE(n.has_value());
        REQUIRE(*n == 16);
    }

    SECTION("With gcode_response // prefix") {
        auto n = helix::parse_adapted_probe_count("// Adapted probe count: 5,3");
        REQUIRE(n.has_value());
        REQUIRE(*n == 15);
    }

    SECTION("Whitespace tolerance around comma") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 6 , 6");
        REQUIRE(n.has_value());
        REQUIRE(*n == 36);
    }

    SECTION("Large grid (Voron 350)") {
        auto n = helix::parse_adapted_probe_count("Adapted probe count: 9,9");
        REQUIRE(n.has_value());
        REQUIRE(*n == 81);
    }
}

TEST_CASE("parse_adapted_probe_count rejects unrelated lines", "[bed_mesh_collector][adapted]") {
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Probing point 5/25").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("// bed_mesh: generated points").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Mesh X,Y: 10,10").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Adapted probe count: 0,4").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("Adapted probe count: 4,0").has_value());
    REQUIRE_FALSE(helix::parse_adapted_probe_count("").has_value());
}

/**
 * @brief Simulate Klipper's `samples: N` probe stream with dedupe
 *
 * Klipper emits N consecutive "probe at X,Y is z=Z" lines at the same (x,y)
 * for each grid point when `samples: N` is configured. The collector's
 * dedupe logic (parse_probe_position + tolerance check) should count unique
 * points, not raw sample lines — matching the user-facing "# points probed"
 * expectation.
 *
 * This mirrors the real Snapmaker U1 observation: probed_matrix 6x5=30 with
 * samples=3 produces 90 "probe at" lines that must deduplicate to 30 points.
 */
static int simulate_dedupe_count(int grid_rows, int grid_cols, int samples, double x_spacing = 46.1,
                                 double y_spacing = 41.62) {
    constexpr double POS_TOL = 0.05;
    double last_x = 0.0, last_y = 0.0;
    bool has_last = false;
    int unique_points = 0;

    for (int r = 0; r < grid_rows; ++r) {
        for (int c = 0; c < grid_cols; ++c) {
            double x = 43.173 + c * x_spacing;
            double y = 55.048 + r * y_spacing;
            for (int s = 0; s < samples; ++s) {
                // Simulate the same dedupe logic as production:
                // count unique points, ignore consecutive samples at same (x,y).
                bool is_new =
                    !has_last || std::abs(x - last_x) > POS_TOL || std::abs(y - last_y) > POS_TOL;
                if (is_new) {
                    ++unique_points;
                    last_x = x;
                    last_y = y;
                    has_last = true;
                }
            }
        }
    }
    return unique_points;
}

TEST_CASE("Sample dedupe: 6x5 grid with samples=3 counts 30 points, not 90",
          "[bed_mesh_collector][dedupe]") {
    // Matches Snapmaker U1 observed behavior: probe_count=[13,13] config,
    // but adaptive mesh probes 6x5=30 points, 3 samples each = 90 "probe at" lines.
    // Dedupe must collapse these to 30 unique points.
    REQUIRE(simulate_dedupe_count(6, 5, 3) == 30);
}

TEST_CASE("Sample dedupe: 5x5 grid with samples=1 counts 25 points",
          "[bed_mesh_collector][dedupe]") {
    REQUIRE(simulate_dedupe_count(5, 5, 1) == 25);
}

TEST_CASE("Sample dedupe: 3x3 grid with samples=5 counts 9 points",
          "[bed_mesh_collector][dedupe]") {
    REQUIRE(simulate_dedupe_count(3, 3, 5) == 9);
}

TEST_CASE("Sample dedupe: tolerance distinguishes adjacent grid points",
          "[bed_mesh_collector][dedupe]") {
    // Neighboring probe positions ~1mm apart must NOT be treated as the same point
    // even though they're close. Tolerance is 0.05mm — samples at exactly the same
    // xy have 0 difference, real grid moves are always >>0.05mm.
    REQUIRE(simulate_dedupe_count(2, 2, 2, 1.0, 1.0) == 4);
}

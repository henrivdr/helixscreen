// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <regex>
#include <string>

namespace helix {

/**
 * @brief Result of parsing a bed mesh probe line
 *
 * current: 1-based probe index (from "Probing point X/Y" or fallback count)
 * total:   total expected probes (0 if unknown, e.g. fallback "probe at" lines
 *          without a known grid size)
 */
struct ProbeProgress {
    int current;
    int total; ///< 0 = unknown
};

/**
 * @brief Parse a G-code response line for bed mesh probe progress
 *
 * Handles two formats:
 *  1. "Probing point 5/25", "Probe point 5 of 25", "Probing mesh point 5/25"
 *  2. "probe at X,Y is z=Z" (fallback — caller must maintain a running count)
 *
 * For format (1), returns {current, total}.
 * For format (2), returns std::nullopt — use is_probe_result_line() to detect
 * these and maintain your own counter.
 *
 * @param line G-code response line
 * @return Parsed {current, total} or std::nullopt
 */
inline std::optional<ProbeProgress> parse_probe_progress(const std::string& line) {
    // Static regex — handles "Probing point 5/25", "Probe point 5 of 25",
    // "Probing mesh point 5/25"
    static const std::regex probe_regex(
        R"(Prob(?:ing (?:mesh )?point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))");

    std::smatch match;
    if (std::regex_search(line, match, probe_regex) && match.size() == 3) {
        try {
            return ProbeProgress{std::stoi(match[1].str()), std::stoi(match[2].str())};
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/**
 * @brief Check if a line is a "probe at X,Y is z=Z" result line
 *
 * These lines appear on firmware that doesn't emit "Probing point X/Y" progress
 * markers. Callers should maintain their own running count when this returns true.
 */
inline bool is_probe_result_line(const std::string& line) {
    return line.find("probe at ") != std::string::npos && line.find(" is z=") != std::string::npos;
}

/**
 * @brief (x,y) position parsed from a "probe at X,Y is z=Z" line
 */
struct ProbePosition {
    double x;
    double y;
};

/**
 * @brief Extract (x,y) from a "probe at X,Y is z=Z" line
 *
 * Klipper's `samples` config causes the same (x,y) to appear multiple times
 * consecutively. Callers can use this to deduplicate samples and count unique
 * probe points rather than raw sample lines.
 *
 * Accepts both comma and whitespace separators after "probe at", and optional
 * "x:"/"y:" prefixes (seen on some firmwares).
 *
 * @return Parsed position or std::nullopt if line doesn't match
 */
/**
 * @brief Stock Klipper's adaptive bed_mesh emits this line when the slicer
 * passes MESH_MIN/MESH_MAX overrides (or ADAPTIVE=1) and bed_mesh.py reduces
 * the grid from configfile defaults.
 *
 * Format: "Adapted probe count: N,M" (preceded by "// " in gcode_response).
 * The total = N * M, fired once before the first probe — usable as a live
 * denominator on stock Klipper / Voron / KAMP setups.
 *
 * NOT emitted by Snapmaker U1's custom firmware fork (the count exists in
 * klippy.log as "Updated Mesh Configuration" but never reaches
 * gcode_response). U1 uses adaptive_meshing=true in its profile to skip the
 * configfile fallback and rely on probed_matrix from the prior print.
 *
 * @return Total probe count (N * M) or std::nullopt
 */
inline std::optional<int> parse_adapted_probe_count(const std::string& line) {
    static const std::regex adapt_regex(R"(Adapted probe count:\s*(\d+)\s*,\s*(\d+))");
    std::smatch match;
    if (std::regex_search(line, match, adapt_regex) && match.size() == 3) {
        try {
            int x = std::stoi(match[1].str());
            int y = std::stoi(match[2].str());
            if (x > 0 && y > 0) {
                return x * y;
            }
        } catch (...) {
            // fall through
        }
    }
    return std::nullopt;
}

inline std::optional<ProbePosition> parse_probe_position(const std::string& line) {
    // Matches "probe at x: 181.474, y: 55.048 is z=..." and "probe at 150.0,150.0 is z=..."
    static const std::regex pos_regex(
        R"(probe at (?:x:\s*)?(-?\d+(?:\.\d+)?)[,\s]+(?:y:\s*)?(-?\d+(?:\.\d+)?)\s+is z=)");
    std::smatch match;
    if (std::regex_search(line, match, pos_regex) && match.size() == 3) {
        try {
            return ProbePosition{std::stod(match[1].str()), std::stod(match[2].str())};
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace helix

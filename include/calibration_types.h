// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

/// Minutes of arc under which a screw is considered in-spec and shown as level.
constexpr int SCREW_LEVEL_TOLERANCE_MINUTES = 5;

/**
 * @brief Flip the leading CW↔CCW direction token in an adjustment string
 *
 * Rewrites `"CW 01:15"` → `"CCW 01:15"` and `"CCW 00:30"` → `"CW 00:30"`
 * in place. No-op if the string doesn't begin with a direction token (e.g.
 * already stripped, empty, or malformed). Used by the screws-tilt parser
 * to apply the printer-database `screws_tilt_direction` override.
 */
inline void flip_screws_tilt_direction(std::string& adjustment) {
    if (adjustment.rfind("CCW", 0) == 0) {
        adjustment.replace(0, 3, "CW");
    } else if (adjustment.rfind("CW", 0) == 0) {
        adjustment.replace(0, 2, "CCW");
    }
}

/**
 * @file calibration_types.h
 * @brief Data structures for printer calibration features
 *
 * Types for bed leveling, input shaping, and machine limits.
 * Used by the screws tilt panel, input shaper panel, and calibration wizards.
 */

// ============================================================================
// Bed Leveling Types
// ============================================================================

/**
 * @brief Result from SCREWS_TILT_CALCULATE command
 *
 * Represents a single bed adjustment screw with its measured height
 * deviation and the required adjustment.
 */
struct ScrewTiltResult {
    std::string screw_name; ///< Screw identifier (e.g., "front_left", "rear_right")
    float x_pos = 0.0f;     ///< Bed X coordinate of screw position (mm)
    float y_pos = 0.0f;     ///< Bed Y coordinate of screw position (mm)
    float z_height = 0.0f;  ///< Probed Z height at screw position
    std::string
        adjustment; ///< Adjustment string (e.g., "CW 0:15" for clockwise 0 turns 15 minutes)
    bool is_reference = false; ///< True if this is the reference screw (no adjustment needed)

    /**
     * @brief Check if adjustment is needed
     * @return true if this screw needs turning
     */
    [[nodiscard]] bool needs_adjustment() const {
        return !is_reference && !adjustment.empty() && adjustment != "00:00";
    }

    /**
     * @brief Parsed arc-minute magnitude of this screw's adjustment
     *
     * Parses "CW 01:30" / "CCW 00:15" into total minutes (turns*60 + minutes).
     * Returns 0 for reference screws or unparseable strings.
     */
    [[nodiscard]] int adjustment_minutes() const {
        if (is_reference || adjustment.empty()) {
            return 0;
        }
        int turns = 0;
        int minutes = 0;
        if (std::sscanf(adjustment.c_str(), "%*s %d:%d", &turns, &minutes) == 2) {
            return turns * 60 + minutes;
        }
        return 0;
    }

    /**
     * @brief True if this screw is within the level tolerance (≤5 min or base)
     *
     * Use this — not `!needs_adjustment()` — when deciding whether to show
     * a "done" affordance: a screw reporting `CW 00:03` still has a non-empty
     * adjustment but is effectively level.
     */
    [[nodiscard]] bool is_within_tolerance() const {
        return is_reference || adjustment_minutes() <= SCREW_LEVEL_TOLERANCE_MINUTES;
    }

    /**
     * @brief Get prettified screw name for display
     *
     * Converts snake_case to Title Case (e.g., "front_left" -> "Front Left")
     * @return Human-readable screw name
     */
    [[nodiscard]] std::string display_name() const {
        std::string result;
        bool capitalize_next = true;
        for (char c : screw_name) {
            if (c == '_') {
                result += ' ';
                capitalize_next = true;
            } else if (capitalize_next) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalize_next = false;
            } else {
                result += c;
            }
        }
        return result;
    }

    /**
     * @brief Get user-friendly adjustment description
     *
     * Converts "CW 00:18" to "Tighten 1/4 turn", "CCW 01:30" to "Loosen 1 turn", etc.
     * Mapping is CW→Tighten, CCW→Loosen. This assumes the adjustment string
     * already reflects what the user should physically do — printer-specific
     * corrections (e.g. Flashforge AD5M's inverted SCREWS_TILT output) are
     * applied at parse time in moonraker_advanced_api.cpp, not here.
     * @return Human-friendly adjustment string
     */
    [[nodiscard]] std::string friendly_adjustment() const {
        if (is_reference) {
            return "Reference";
        }
        if (is_within_tolerance()) {
            return "Level";
        }

        bool is_clockwise = adjustment.find("CW") == 0 && adjustment.find("CCW") != 0;
        bool is_counter = adjustment.find("CCW") == 0;
        int total_minutes = adjustment_minutes();

        std::string amount;
        if (total_minutes <= 10) {
            amount = "1/8 turn";
        } else if (total_minutes <= 20) {
            amount = "1/4 turn";
        } else if (total_minutes <= 35) {
            amount = "1/2 turn";
        } else if (total_minutes <= 50) {
            amount = "3/4 turn";
        } else if (total_minutes <= 70) {
            amount = "1 turn";
        } else {
            int approx_turns = (total_minutes + 30) / 60;
            amount = std::to_string(approx_turns) + " turn" + (approx_turns > 1 ? "s" : "");
        }

        if (is_clockwise) {
            return "Tighten " + amount;
        } else if (is_counter) {
            return "Loosen " + amount;
        }
        return adjustment;
    }
};

/**
 * @brief Bed leveling method selection
 */
enum class BedLevelingMethod {
    AUTO_MESH,     ///< BED_MESH_CALIBRATE - Automatic probing grid
    MANUAL_SCREWS, ///< SCREWS_TILT_CALCULATE - Manual screw adjustment guidance
    QUAD_GANTRY,   ///< QUAD_GANTRY_LEVEL - Voron-style gantry leveling
    Z_TILT         ///< Z_TILT_ADJUST - Multi-motor Z adjustment
};

// ============================================================================
// Input Shaping Types
// ============================================================================

// Forward declaration for all_shapers vector
struct ShaperOption;

/**
 * @brief Per-shaper frequency response curve from calibration CSV
 *
 * Contains the filtered PSD response for one shaper type at all frequency bins.
 * Used for overlaying shaper response on the raw frequency spectrum chart.
 */
struct ShaperResponseCurve {
    std::string name;          ///< Shaper type (e.g., "zv", "mzv", "ei")
    float frequency = 0.0f;    ///< Fitted frequency in Hz (from CSV header)
    std::vector<float> values; ///< Filtered PSD values at each frequency bin
};

/**
 * @brief Result from resonance testing (TEST_RESONANCES or Klippain)
 *
 * Contains the recommended shaper configuration for one axis, plus
 * all fitted shaper alternatives for comparison.
 */
struct InputShaperResult {
    char axis = 'X';          ///< Axis tested ('X' or 'Y')
    std::string shaper_type;  ///< Recommended shaper (e.g., "mzv", "ei", "2hump_ei", "3hump_ei")
    float shaper_freq = 0.0f; ///< Recommended frequency in Hz
    float max_accel = 0.0f;   ///< Maximum recommended acceleration in mm/s²
    float smoothing = 0.0f;   ///< Smoothing value (0.0-1.0, lower is better)
    float vibrations = 0.0f;  ///< Remaining vibrations percentage

    /// Path to CSV calibration data file (e.g., /tmp/calibration_data_x_*.csv)
    std::string csv_path;

    /// True when Klipper reported a CSV path but its frequency-response data
    /// could not be read (missing/unreadable file — e.g. systemd PrivateTmp
    /// isolation hiding Klipper's /tmp output — or a malformed CSV). The
    /// recommendation is still valid; only the chart is unavailable.
    bool chart_data_unavailable = false;

    /// Frequency response data for graphing (frequency Hz, amplitude)
    std::vector<std::pair<float, float>> freq_response;

    /// Per-shaper filtered response curves (for chart overlay)
    std::vector<ShaperResponseCurve> shaper_curves;

    /// All fitted shaper options from calibration (not just recommended)
    std::vector<ShaperOption> all_shapers;

    /// Check if frequency response data is available for charting
    [[nodiscard]] bool has_freq_data() const {
        return !freq_response.empty();
    }

    /**
     * @brief Check if result contains valid data
     */
    [[nodiscard]] bool is_valid() const {
        return !shaper_type.empty() && shaper_freq > 0.0f;
    }
};

/**
 * @brief Single shaper option with all metrics
 *
 * Represents one fitted shaper from resonance testing, with complete
 * metrics for comparison. Used in the all_shapers vector of InputShaperResult.
 */
struct ShaperOption {
    std::string type;        ///< Shaper type (e.g., "zv", "mzv", "ei", "2hump_ei", "3hump_ei")
    float frequency = 0.0f;  ///< Fitted frequency in Hz
    float vibrations = 0.0f; ///< Remaining vibrations percentage (lower is better)
    float smoothing = 0.0f;  ///< Smoothing value (lower is sharper corners)
    float max_accel = 0.0f;  ///< Maximum recommended acceleration in mm/s²
};

/**
 * @brief Current input shaper configuration from printer state
 *
 * Represents the currently active input shaper settings as configured
 * in Klipper. Retrieved via printer.objects.query for input_shaper.
 */
struct InputShaperConfig {
    std::string shaper_type_x;    ///< Active shaper type for X axis (empty if not configured)
    float shaper_freq_x = 0.0f;   ///< Active frequency for X axis in Hz
    std::string shaper_type_y;    ///< Active shaper type for Y axis (empty if not configured)
    float shaper_freq_y = 0.0f;   ///< Active frequency for Y axis in Hz
    float damping_ratio_x = 0.0f; ///< Damping ratio for X axis (default 0.1)
    float damping_ratio_y = 0.0f; ///< Damping ratio for Y axis (default 0.1)
    bool is_configured = false;   ///< True if input shaper is actively configured
};

// ============================================================================
// Machine Limits Types
// ============================================================================

/**
 * @brief Printer motion limits (velocity, acceleration)
 *
 * Represents current or target machine limits. Can be applied temporarily
 * via SET_VELOCITY_LIMIT or permanently via SAVE_CONFIG.
 */
struct MachineLimits {
    double max_velocity = 0;           ///< Maximum velocity in mm/s
    double max_accel = 0;              ///< Maximum acceleration in mm/s²
    double max_accel_to_decel = 0;     ///< Maximum acceleration to deceleration in mm/s²
    double square_corner_velocity = 0; ///< Square corner velocity in mm/s
    double max_z_velocity = 0;         ///< Maximum Z velocity in mm/s
    double max_z_accel = 0;            ///< Maximum Z acceleration in mm/s²

    /**
     * @brief Check if limits contain valid data
     */
    [[nodiscard]] bool is_valid() const {
        return max_velocity > 0 && max_accel > 0;
    }

    /**
     * @brief Compare two limit sets for equality
     */
    [[nodiscard]] bool operator==(const MachineLimits& other) const {
        return max_velocity == other.max_velocity && max_accel == other.max_accel &&
               max_accel_to_decel == other.max_accel_to_decel &&
               square_corner_velocity == other.square_corner_velocity &&
               max_z_velocity == other.max_z_velocity && max_z_accel == other.max_z_accel;
    }

    [[nodiscard]] bool operator!=(const MachineLimits& other) const {
        return !(*this == other);
    }
};

// ============================================================================
// Calibration Callback Types
// ============================================================================

namespace helix {
/// Bed screw results callback
using ScrewTiltCallback = std::function<void(const std::vector<ScrewTiltResult>&)>;

/// Input shaper result callback
using InputShaperCallback = std::function<void(const InputShaperResult&)>;

/// Machine limits callback
using MachineLimitsCallback = std::function<void(const MachineLimits&)>;
} // namespace helix

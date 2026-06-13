// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "unit_conversions.h"

#include <cstddef>
#include <string>

/**
 * @file ui_temperature_utils.h
 * @brief Shared temperature validation, formatting, and display utilities
 *
 * This module provides centralized temperature validation, clamping,
 * formatting, and color-coding logic used across multiple temperature-related
 * panels (controls/temp, filament, extrusion).
 *
 * ## Formatting Functions
 *
 * Use these for consistent temperature display across the UI:
 * - `format_temperature()` - Single temp: "210°C"
 * - `format_temperature_pair()` - Current/target: "210 / 245°C"
 *
 * ## Color-Coding Function
 *
 * Use `get_heating_state_color()` for consistent 4-state thermal feedback:
 * - Off (target=0): gray (text_muted)
 * - Heating (current < target-2): red (primary_color)
 * - At-temp (within ±2): green (success_color)
 * - Cooling (current > target+2): blue (info_color)
 */

namespace helix {
namespace ui {
namespace temperature {

// ============================================================================
// Unit Conversion Functions
// ============================================================================

/**
 * @brief Converts centidegrees to degrees (integer)
 *
 * PrinterState stores temperatures as centidegrees (×10) for 0.1°C resolution.
 * Use this function for integer display (e.g., "210°C").
 *
 * @param centi Temperature in centidegrees (e.g., 2100 for 210°C)
 * @return Temperature in degrees (e.g., 210)
 */
inline int centi_to_degrees(int centi) {
    return static_cast<int>(helix::units::from_centidegrees(centi));
}

/**
 * @brief Converts centidegrees to degrees (float for precision display)
 *
 * Use this function when 0.1°C precision is needed (e.g., graph data points).
 *
 * @param centi Temperature in centidegrees (e.g., 2105 for 210.5°C)
 * @return Temperature in degrees (e.g., 210.5f)
 */
inline float centi_to_degrees_f(int centi) {
    return static_cast<float>(helix::units::from_centidegrees(centi));
}

/**
 * @brief Converts degrees to centidegrees
 *
 * Use when setting temperatures from user input (e.g., keyboard entry).
 *
 * @param degrees Temperature in degrees (e.g., 210)
 * @return Temperature in centidegrees (e.g., 2100)
 */
inline int degrees_to_centi(int degrees) {
    return helix::units::to_centidegrees(static_cast<double>(degrees));
}

// ============================================================================
// Validation Functions
// ============================================================================

/**
 * @brief Validates and clamps a temperature value to safe limits
 *
 * If the temperature is outside the valid range, it will be clamped to
 * the nearest valid value and a warning will be logged.
 *
 * @param temp Temperature value to validate (modified in-place if clamped)
 * @param min_temp Minimum valid temperature
 * @param max_temp Maximum valid temperature
 * @param context Logging context (e.g., "Temp", "Filament", "Extrusion")
 * @param temp_type Temperature type for logging (e.g., "current", "target")
 * @return true if temperature was valid, false if it was clamped
 */
bool validate_and_clamp(int& temp, int min_temp, int max_temp, const char* context,
                        const char* temp_type);

/**
 * @brief Validates and clamps a temperature pair (current + target)
 *
 * Convenience function that validates both current and target temperatures.
 *
 * @param current Current temperature (modified in-place if clamped)
 * @param target Target temperature (modified in-place if clamped)
 * @param min_temp Minimum valid temperature
 * @param max_temp Maximum valid temperature
 * @param context Logging context (e.g., "Temp", "Filament")
 * @return true if both temperatures were valid, false if either was clamped
 */
bool validate_and_clamp_pair(int& current, int& target, int min_temp, int max_temp,
                             const char* context);

/**
 * @brief Checks if the current temperature is safe for extrusion
 *
 * Extrusion operations require the nozzle to be at or above a minimum
 * temperature (typically 170°C) to avoid damaging the extruder.
 *
 * @param current_temp Current nozzle temperature
 * @param min_extrusion_temp Minimum safe extrusion temperature
 * @return true if safe to extrude, false otherwise
 */
bool is_extrusion_safe(int current_temp, int min_extrusion_temp);

/**
 * @brief Gets a human-readable safety status message
 *
 * @param current_temp Current nozzle temperature
 * @param min_extrusion_temp Minimum safe extrusion temperature
 * @return Status message (e.g., "Ready" or "Heating (45°C below minimum)")
 */
const char* get_extrusion_safety_status(int current_temp, int min_extrusion_temp);

// ============================================================================
// Formatting Functions
// ============================================================================

/**
 * @brief Format a temperature value with degree symbol
 *
 * Formats as "210°C" for consistent display across the UI.
 *
 * @param temp Temperature in degrees (not centidegrees)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 *
 * @code{.cpp}
 * char buf[16];
 * lv_label_set_text(label, format_temperature(210, buf, sizeof(buf)));
 * @endcode
 */
char* format_temperature(int temp, char* buffer, size_t buffer_size);

/**
 * @brief Format a current/target temperature pair
 *
 * Formats as "210 / 245°C" or "210 / —°C" when target is 0 (heater off).
 *
 * @param current Current temperature in degrees
 * @param target Target temperature in degrees (0 = heater off, shows "—")
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 24)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_pair(int current, int target, char* buffer, size_t buffer_size);

/**
 * @brief Format a target temperature or "— °C" when off
 *
 * Formats as "245°C" when target > 0, or "— °C" when target is 0 (heater off).
 *
 * @param target Target temperature in degrees (0 = heater off)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_target_or_off(int target, char* buffer, size_t buffer_size);

/**
 * @brief Format a temperature value with one decimal place
 *
 * Formats as "210.5°C" for precision display (graphs, PID tuning).
 *
 * @param temp Temperature in degrees (float)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_f(float temp, char* buffer, size_t buffer_size);

/**
 * @brief Format a float current/target temperature pair
 *
 * Formats as "210.5 / 215.0°C" or "180.5 / —°C" when target is 0.
 *
 * @param current Current temperature in degrees (float)
 * @param target Target temperature in degrees (float, 0 = heater off)
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 32)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_pair_f(float current, float target, char* buffer, size_t buffer_size);

/**
 * @brief Format a temperature range for material specs
 *
 * Formats as "200-230°C" for AMS material temperature ranges.
 *
 * @param min_temp Minimum temperature in degrees
 * @param max_temp Maximum temperature in degrees
 * @param buffer Output buffer
 * @param buffer_size Size of buffer (recommended: 16)
 * @return Pointer to buffer for chaining convenience
 */
char* format_temperature_range(int min_temp, int max_temp, char* buffer, size_t buffer_size);

// ============================================================================
// Display Color Functions
// ============================================================================

/** Default tolerance for "at temperature" state detection (±degrees) */
constexpr int DEFAULT_AT_TEMP_TOLERANCE = 2;

/**
 * @brief Get theme color for temperature display based on 4-state heating logic
 *
 * Returns a color indicating the thermal state of a heater:
 * - **Off** (target=0): text_muted (gray) - heater disabled
 * - **Heating** (current < target - tolerance): primary_color (red) - actively heating
 * - **At-temp** (within ±tolerance): success_color (green) - stable at target
 * - **Cooling** (current > target + tolerance): info_color (blue) - cooling down
 *
 * This function provides consistent color-coding across all temperature displays
 * (temp_display widget, filament panel, etc.).
 *
 * @param current_deg Current temperature in degrees
 * @param target_deg Target temperature in degrees (0 = heater off)
 * @param tolerance Degrees tolerance for "at temp" state (default: 2)
 * @return lv_color_t Theme color based on thermal state
 *
 * @code{.cpp}
 * lv_color_t color = get_heating_state_color(nozzle_current, nozzle_target);
 * lv_obj_set_style_text_color(temp_label, color, LV_PART_MAIN);
 * @endcode
 */
lv_color_t get_heating_state_color(int current_deg, int target_deg,
                                   int tolerance = DEFAULT_AT_TEMP_TOLERANCE);

// ============================================================================
// Heater Display
// ============================================================================

/**
 * @brief Result of formatting a heater display
 *
 * Contains all the information needed to display a heater status:
 * - temp: formatted temperature string (e.g., "150°C" or "150 / 200°C")
 * - status: semantic status ("Off", "Heating...", "Ready", or "Cooling")
 * - pct: percentage towards target (0-100, clamped)
 * - color: theme color matching the heating state (from get_heating_state_color)
 */
struct HeaterDisplayResult {
    std::string temp;
    std::string status;
    int pct;
    lv_color_t color;
};

/**
 * @brief Format heater display information from centi-degree values
 *
 * Takes current and target temperatures in centi-degrees (100 = 1°C) and
 * produces a consistent display result used across all heater displays.
 * Includes a color field from get_heating_state_color() for one-call convenience.
 *
 * Status logic (DEFAULT_AT_TEMP_TOLERANCE, matches get_heating_state_color):
 * - target <= 0: "Off"
 * - current < target - tolerance: "Heating..."
 * - current > target + tolerance: "Cooling"
 * - within +/- tolerance: "Ready"
 *
 * @param current_centi Current temperature in centi-degrees
 * @param target_centi Target temperature in centi-degrees (0 = off)
 * @return HeaterDisplayResult with formatted temp, status, percentage, and color
 */
HeaterDisplayResult heater_display(int current_centi, int target_centi);

// ============================================================================
// Heater GCode
// ============================================================================

/**
 * @brief Build gcode to set a heater temperature (any type)
 *
 * Handles all Klipper heater types:
 * - "temperature_fan X" → SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=X
 * - "heater_generic X"  → SET_HEATER_TEMPERATURE HEATER=X
 * - "extruder"          → SET_HEATER_TEMPERATURE HEATER=extruder
 * - "heater_bed"        → SET_HEATER_TEMPERATURE HEATER=heater_bed
 *
 * @param heater_full_name Full Klipper heater name
 * @param target_centi     Target temperature in centidegrees (x10, e.g. 2100 = 210°C)
 * @param buffer           Output buffer
 * @param buffer_size      Size of buffer
 * @param use_m141         When true, emit "M141 S{deg}" instead of a raw
 *                         SET_HEATER_TEMPERATURE/SET_TEMPERATURE_FAN_TARGET command
 * @return Pointer to buffer, or nullptr if heater_full_name is empty
 */
const char* build_heater_gcode(const std::string& heater_full_name, int target_centi, char* buffer,
                               size_t buffer_size, bool use_m141 = false);

/**
 * @brief Decide whether a chamber temperature command should route through M141.
 *
 * True when a chamber temperature command should route through the standard
 * M141 macro instead of a raw SET_HEATER_TEMPERATURE: the target heater is the
 * discovered chamber heater AND the printer defines an M141 macro.
 */
bool chamber_uses_m141(const std::string& heater_full_name,
                       const std::string& chamber_heater_name, bool m141_available);

/**
 * @brief Effective chamber setpoint and control mode from the two M141 targets.
 *
 * The K2's M141 macro splits the chamber setpoint across two Klipper objects:
 * a HEATING setpoint (>40°C) lands on the heater target, while a MAINTAINING
 * setpoint (≤40°C) lands on the cooling-fan target with the heater target at 0.
 * On printers without a chamber cooling fan the fan target stays 0, so this
 * reduces to the heater target (Heating/Off only) — safe and universal.
 *
 * @param heater_target_centi Chamber heater target (×10; 0 = not heating)
 * @param fan_target_centi    Chamber cooling-fan target (×10; 0 = not maintaining)
 * @return centi = effective setpoint (×10), mode = "Heating" | "Maintaining" | "Off"
 */
struct ChamberSetpoint {
    int centi;
    const char* mode; ///< "Heating" | "Maintaining" | "Off"
};

ChamberSetpoint chamber_effective_setpoint(int heater_target_centi, int fan_target_centi);

/**
 * @brief Build gcode to turn off a heater (target=0)
 *
 * Convenience wrapper for build_heater_gcode with target=0.
 */
inline const char* build_heater_off_gcode(const std::string& heater_full_name, char* buffer,
                                          size_t buffer_size) {
    return build_heater_gcode(heater_full_name, 0, buffer, buffer_size);
}

} // namespace temperature
} // namespace ui
} // namespace helix

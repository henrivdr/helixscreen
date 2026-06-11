// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file heater_limits.h
 * @brief Pure decisions for clamping a heater panel's keypad range and presets
 *        to the printer's Klipper-configured max_temp.
 *
 * Used by the chamber temperature panel so the custom-temp keypad and the
 * preset buttons honor the real hardware ceiling (e.g. a 60°C chamber) instead
 * of a hardcoded default. Kept free of LVGL so the logic is unit-testable on
 * its own.
 *
 * Convention: a configured maximum of <= 0 means "unknown" — the value has not
 * yet been read from the printer's configfile — and the panel default applies.
 */

#pragma once

namespace helix {

/**
 * @brief Effective temperature ceiling (°C) for a heater's keypad and presets.
 *
 * @param default_max_deg    The panel's built-in ceiling (e.g. 80°C for chamber).
 * @param configured_max_deg Klipper-configured max_temp in °C; <= 0 means unknown.
 * @return configured_max_deg when known (> 0), otherwise default_max_deg.
 */
inline float heater_effective_max_deg(float default_max_deg, int configured_max_deg) {
    return configured_max_deg > 0 ? static_cast<float>(configured_max_deg) : default_max_deg;
}

/**
 * @brief Whether a preset target (°C) should be shown given the configured max.
 *
 * The "off" preset (0°C) is always shown. A preset exactly at the configured
 * max is shown (inclusive). When the configured max is unknown (<= 0), all
 * presets are shown.
 *
 * @param preset_deg         Preset target in °C (0 = off).
 * @param configured_max_deg Klipper-configured max_temp in °C; <= 0 means unknown.
 */
inline bool heater_preset_visible(int preset_deg, int configured_max_deg) {
    if (configured_max_deg <= 0) {
        return true; // unknown ceiling — show everything
    }
    return preset_deg <= configured_max_deg;
}

} // namespace helix

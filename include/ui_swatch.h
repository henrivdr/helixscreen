// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <string>

/**
 * @file ui_swatch.h
 * @brief Flat filament-color swatch painting with multi-color support.
 *
 * A "flat swatch" is a plain lv_obj whose background represents a filament
 * color. Single-color spools render as a solid fill with a subtle delineating
 * border (so near-black colors stay visible on dark backgrounds). Multi-color
 * spools (Spoolman `multi_color_hexes`, RFID co-extrusion, etc.) render as
 * equal-area diagonal chunks: two colors split the square into two triangles
 * along the anti-diagonal, three colors give triangle / stripe / triangle, and
 * so on for N colors.
 *
 * The multi-color path installs a lightweight DRAW_MAIN callback that emits a
 * handful of lv_draw_triangle tasks — no per-swatch canvas buffer, so it is
 * safe on memory-constrained devices.
 */

namespace helix::ui {

/**
 * Paint a flat color swatch.
 *
 * @param swatch             the swatch object (its background is (re)styled)
 * @param primary_rgb        packed 0xRRGGBB used when there is no multi-color list
 * @param multi_color_hexes  comma-separated hex colors (optionally '#'-prefixed);
 *                           empty or single-entry falls back to a solid fill
 *
 * Safe to call repeatedly on the same object (e.g. from an observer): the
 * multi-color state is updated in place and the swatch invalidated. Must be
 * called from the main (UI) thread.
 */
void apply_swatch_color(lv_obj_t* swatch, uint32_t primary_rgb,
                        const std::string& multi_color_hexes);

/**
 * Drop all retained multi-color swatch state. Normally unnecessary (state is
 * released when each swatch is deleted); exposed for test teardown.
 */
void reset_swatch_state();

} // namespace helix::ui

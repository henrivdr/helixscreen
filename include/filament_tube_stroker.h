// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_tube_stroker.h
 * @brief Shared concentric tube stroker for AMS filament-path rendering.
 *
 * A "tube" is rendered as a set of CONCENTRIC stroke passes (no perpendicular
 * offsets) over a pathgeo::FilamentPath. Straight runs are drawn with
 * lv_draw_line (float coords — we build with LV_USE_FLOAT) and corners as a fan
 * of short straight chords sampled from the arc's exact float parametrization
 * (also lv_draw_line, NOT lv_draw_arc, whose integer center / outer radius would
 * round the band ~0.5px out of alignment with the adjoining lines). Round caps
 * live only on the very first seg-start and last seg-end of the path; interior
 * joints (including chord-to-chord) use butt caps (segments join tangentially,
 * so butt joints are seamless and translucent passes don't double-blend).
 *
 * This layer is shared by both AMS path canvases:
 *   - ui_filament_path_canvas (single-unit detail panel)
 *   - ui_system_path_canvas   (multi-unit overview)
 * so both produce identical clean quarter-arc fillet curves. Callers supply
 * lane width / color choices via LaneStyle (the overview's lanes are
 * intentionally thinner/dimmer than the detail panel).
 */

#include "filament_path_geometry.h"
#include "lvgl/lvgl.h"

namespace helix {
namespace ui {

namespace pg = pathgeo;

// Centerline fillet radius for orthogonal lane routing (route_orthogonal clamps
// internally, so tight layouts degrade gracefully to jogs / straight runs).
inline constexpr float FILLET_RADIUS = 12.0f;

// Soft bloom behind active filament paths. A wide, low-opacity line in a lighter
// tint of the filament color.
inline constexpr lv_opa_t GLOW_OPA = 60;       // Base glow opacity
inline constexpr int32_t GLOW_WIDTH_EXTRA = 6; // Extra width beyond tube on each side

/// Detect low-performance platforms (K1/K2/MIPS or constrained-memory devices).
/// When true, the stroker drops the glow / outline / core-highlight passes and
/// renders a single body pass, matching the legacy "simple" tube look.
bool reduced_effects();

/// Color helpers (delegate to ams_draw::* — kept here so callers and the
/// stroker share one definition).
lv_color_t tube_darken(lv_color_t c, uint8_t amt);
lv_color_t tube_lighten(lv_color_t c, uint8_t amt);
lv_color_t tube_blend(lv_color_t c1, lv_color_t c2, float factor);

/// Get a suitable glow color from a filament color. Very dark filaments get a
/// contrasting blue tint so the glow is still visible.
lv_color_t get_glow_color(lv_color_t color);

// One concentric stroke pass.
struct TubePass {
    lv_color_t color;
    int32_t width;
    lv_opa_t opa;
};

// Build-from-state descriptor for a lane's tube.
struct LaneStyle {
    bool solid;       // solid filament tube vs hollow idle PTFE
    lv_color_t color; // filament color (solid) or idle wall color (hollow)
    lv_color_t bg;    // background for hollow bore
    int32_t width;    // tube outer width
    bool glow;        // wide low-opacity backdrop (active lanes only)
};

/// Stroke a path with N concentric passes.
void stroke_path(lv_layer_t* layer, const pg::FilamentPath& path, const TubePass* passes,
                 int n_passes);

/// Build the concentric pass list for a LaneStyle. Returns the number of passes
/// written into @p out (caller sizes out for at least 4).
int build_passes(const LaneStyle& style, TubePass* out);

/// Build a LaneStyle from slot state in ONE place.
LaneStyle lane_style(bool has_filament, lv_color_t tool_color, lv_color_t idle_color, lv_color_t bg,
                     int32_t active_w);

/// Draw a path with a style; optionally record (append) the path's segments into
/// @p record so a flow-dot / tip animation walks exactly what was drawn.
void draw_lane(lv_layer_t* layer, const pg::FilamentPath& path, const LaneStyle& style,
               pg::FilamentPath* record = nullptr);

/// Convenience: vertical tube run from (x,y0) to (x,y1). Only records forward
/// (downward) verticals into @p record (preserves the legacy guard).
void draw_lane_vline(lv_layer_t* layer, int32_t x, int32_t y0, int32_t y1, const LaneStyle& style,
                     pg::FilamentPath* record = nullptr);

/// Convenience: orthogonal route (vertical -> fillet -> horizontal -> fillet ->
/// vertical) from (x0,y0) down to (x1,y1).
void draw_lane_route(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     float fillet_r, const LaneStyle& style, pg::FilamentPath* record = nullptr);

/// Convenience: horizontal tube run from (x0,y) to (x1,y).
void draw_lane_hline(lv_layer_t* layer, int32_t x0, int32_t x1, int32_t y, const LaneStyle& style,
                     pg::FilamentPath* record = nullptr);

// One lane in a hub merge fan: its source column / diagonal-start height, draw
// style, and an optional active-path record sink. Used by draw_merge_fan.
struct MergeFanLane {
    int32_t slot_x;
    int32_t start_y;
    LaneStyle style;
    pg::FilamentPath* record = nullptr; // append the drawn path here (active lane)
};

// Shared hub-merge renderer: parallel diagonals per side, separation by
// construction (see pathgeo::build_merge_fan). Computes widened hub-top entries
// + one common slope per side so no two lanes ever overlap or pinch, then routes
// and draws each lane. Consumed by BOTH AMS path canvases (detail HUB renderer,
// detail mixed-topology, overview multi-tool routes, overview single-tool hub
// convergence). @p hub_top is the verticals' final destination; @p fillet_r is
// the corner-fillet radius (8 on the detail panel, 9 on the overview).
//
// If @p entry_x_out is non-null it receives each lane's computed hub-top entry x
// (size >= n) so the caller can place sensor dots exactly where the tubes land.
void draw_merge_fan(lv_layer_t* layer, const MergeFanLane* lanes, int n, int32_t hub_cx,
                    int32_t hub_top, int32_t hub_w, float fillet_r, int32_t* entry_x_out = nullptr);

} // namespace ui
} // namespace helix

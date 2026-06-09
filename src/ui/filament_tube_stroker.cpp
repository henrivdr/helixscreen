// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_tube_stroker.h"

#include "memory_utils.h"
#include "ui/ams_drawing_utils.h"

#include <cmath>

namespace helix {
namespace ui {

// Detect low-performance platforms at compile time or runtime.
// Returns true on K1/K2/MIPS (weak CPUs) or constrained-memory devices.
bool reduced_effects() {
#if defined(HELIX_PLATFORM_K2) || defined(HELIX_PLATFORM_MIPS)
    return true;
#else
    static const bool cached = helix::get_system_memory_info().is_constrained_device();
    return cached;
#endif
}

// Color helpers delegate to ams_draw so there is one definition of the
// darken/lighten/blend math across the AMS drawing code.
lv_color_t tube_darken(lv_color_t c, uint8_t amt) {
    return ams_draw::darken_color(c, amt);
}

lv_color_t tube_lighten(lv_color_t c, uint8_t amt) {
    return ams_draw::lighten_color(c, amt);
}

lv_color_t tube_blend(lv_color_t c1, lv_color_t c2, float factor) {
    return ams_draw::blend_color(c1, c2, factor);
}

// Get a suitable glow color from a filament color.
lv_color_t get_glow_color(lv_color_t color) {
    // If the filament is very dark, use a contrasting blue tint
    int brightness = color.red + color.green + color.blue;
    if (brightness < 120) {
        return lv_color_hex(0x4466AA); // Dark blue glow for black/dark filaments
    }
    return tube_lighten(color, 60);
}

// Stroke a path with N concentric passes.
void stroke_path(lv_layer_t* layer, const pg::FilamentPath& path, const TubePass* passes,
                 int n_passes) {
    if (path.count <= 0)
        return;

    for (int pi = 0; pi < n_passes; pi++) {
        const TubePass& pass = passes[pi];
        if (pass.width <= 0)
            continue;

        for (int i = 0; i < path.count; i++) {
            const pg::PathSeg& s = path.segs[i];
            bool first = (i == 0);
            bool last = (i == path.count - 1);

            if (s.type == pg::PathSeg::LINE) {
                lv_draw_line_dsc_t dsc;
                lv_draw_line_dsc_init(&dsc);
                dsc.color = pass.color;
                dsc.width = pass.width;
                dsc.opa = pass.opa;
                // Float coords passed directly (LV_USE_FLOAT) — no rounding.
                dsc.p1.x = s.p0.x;
                dsc.p1.y = s.p0.y;
                dsc.p2.x = s.p1.x;
                dsc.p2.y = s.p1.y;
                dsc.round_start = first;
                dsc.round_end = last;
                lv_draw_line(layer, &dsc);
            } else {
                // ARC: lv_draw_arc.radius is the OUTER radius, so add half the
                // stroke width to center the pass on the centerline. center is
                // lv_point_t (int) — round once.
                lv_draw_arc_dsc_t dsc;
                lv_draw_arc_dsc_init(&dsc);
                dsc.color = pass.color;
                dsc.width = pass.width;
                dsc.opa = pass.opa;
                dsc.center.x = (int32_t)lroundf(s.center.x);
                dsc.center.y = (int32_t)lroundf(s.center.y);
                dsc.radius =
                    (uint16_t)LV_MAX(1, (int32_t)lroundf(s.radius + (float)pass.width / 2.0f));

                // LVGL angle convention (verified from
                // lib/lvgl/src/draw/sw/lv_draw_sw_arc.c and lv_draw_sw_mask.c):
                //   - degrees, 0 at 3 o'clock, increasing CLOCKWISE on screen
                //     (matches pathgeo: +x is 0, +y rotates toward larger angles).
                //   - the visible arc is swept clockwise from start_angle to
                //     end_angle; when end < start the sweep wraps through 360
                //     (lv_draw_sw_mask_angle_init: delta = 360 - start + end).
                //   - inputs are NOT wrapped for negatives: lv_draw_sw_arc only
                //     reduces angles >= 360, and lv_draw_sw_mask_angle_init
                //     CLAMPS angles < 0 to 0 (not wrap). A negative start/end
                //     (e.g. pathgeo arc2's a0 = -90deg) therefore collapses to a
                //     bogus ~270deg / full-ring sweep. So both endpoints must be
                //     normalized into [0,360) before handing them to LVGL.
                const float kRad2Deg = 57.29577951308232f;
                auto norm360 = [](float deg) {
                    deg = std::fmod(deg, 360.0f);
                    if (deg < 0.0f)
                        deg += 360.0f;
                    return deg;
                };
                float a0_deg = norm360(s.start_angle * kRad2Deg);
                float a1_deg = norm360((s.start_angle + s.sweep) * kRad2Deg);
                // Order endpoints so LVGL's clockwise (increasing, wrapping)
                // sweep traces the true arc. Positive pathgeo sweep already runs
                // clockwise from start; negative sweep runs from the endpoint.
                // Normalizing each endpoint independently preserves that
                // relationship, and LVGL's end<start wrap selects the short arc.
                if (s.sweep >= 0.0f) {
                    dsc.start_angle = a0_deg;
                    dsc.end_angle = a1_deg;
                } else {
                    dsc.start_angle = a1_deg;
                    dsc.end_angle = a0_deg;
                }
                dsc.rounded = false; // butt ends
                lv_draw_arc(layer, &dsc);
            }
        }
    }
}

// Build the concentric pass list for a LaneStyle. Keeps the darken/lighten
// numbers consistent with the legacy tube look. Returns the number of passes
// written into `out` (caller sizes out for at least 4).
int build_passes(const LaneStyle& style, TubePass* out) {
    const bool simple = reduced_effects();
    int n = 0;
    if (style.solid) {
        if (style.glow && !simple) {
            out[n++] = {get_glow_color(style.color), style.width + GLOW_WIDTH_EXTRA, GLOW_OPA};
        }
        if (!simple) {
            // Outline (darker, slightly wider).
            out[n++] = {tube_darken(style.color, 35), style.width + 2, LV_OPA_COVER};
        }
        // Body (always).
        out[n++] = {style.color, style.width, LV_OPA_COVER};
        if (!simple) {
            // Core highlight (lighter, narrower) — concentric, no offset.
            out[n++] = {tube_lighten(style.color, 44), LV_MAX(1, style.width * 2 / 5),
                        LV_OPA_COVER};
        }
    } else {
        if (!simple) {
            out[n++] = {tube_darken(style.color, 25), style.width + 2, LV_OPA_COVER};
        }
        // Wall.
        out[n++] = {style.color, style.width, LV_OPA_COVER};
        // Bore (background show-through).
        out[n++] = {style.bg, LV_MAX(1, style.width - 2), LV_OPA_COVER};
    }
    return n;
}

// Build a LaneStyle from slot state in ONE place.
LaneStyle lane_style(bool has_filament, lv_color_t tool_color, lv_color_t idle_color, lv_color_t bg,
                     int32_t active_w) {
    LaneStyle st{};
    st.solid = has_filament;
    st.color = has_filament ? tool_color : idle_color;
    st.bg = bg;
    st.width = active_w;
    st.glow = has_filament; // active lanes get the glow backdrop
    return st;
}

// Draw a path with a style; optionally record (append) the path's segments into
// the active-path cache so the flow-dot / tip animation walks exactly what was
// drawn. Geometry is computed once.
void draw_lane(lv_layer_t* layer, const pg::FilamentPath& path, const LaneStyle& style,
               pg::FilamentPath* record) {
    TubePass passes[4];
    int n = build_passes(style, passes);
    stroke_path(layer, path, passes, n);
    if (record) {
        for (int i = 0; i < path.count && record->count < pg::FilamentPath::MAX_SEGS; i++) {
            record->segs[record->count++] = path.segs[i];
        }
    }
}

// Convenience: vertical tube run from (x,y0) to (x,y1).
void draw_lane_vline(lv_layer_t* layer, int32_t x, int32_t y0, int32_t y1, const LaneStyle& style,
                     pg::FilamentPath* record) {
    pg::FilamentPath path;
    path.add_line((float)x, (float)y0, (float)x, (float)y1);
    TubePass passes[4];
    int n = build_passes(style, passes);
    stroke_path(layer, path, passes, n);
    // Preserve the old guard: only record forward (downward) verticals.
    if (record && y1 > y0) {
        for (int i = 0; i < path.count && record->count < pg::FilamentPath::MAX_SEGS; i++)
            record->segs[record->count++] = path.segs[i];
    }
}

// Convenience: orthogonal route (vertical -> fillet -> horizontal -> fillet ->
// vertical) from (x0,y0) down to (x1,y1).
void draw_lane_route(lv_layer_t* layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     float fillet_r, const LaneStyle& style, pg::FilamentPath* record) {
    pg::FilamentPath path;
    pg::route_orthogonal(path, (float)x0, (float)y0, (float)x1, (float)y1, fillet_r);
    draw_lane(layer, path, style, record);
}

// Convenience: horizontal tube run from (x0,y) to (x1,y) (bypass horizontal).
void draw_lane_hline(lv_layer_t* layer, int32_t x0, int32_t x1, int32_t y, const LaneStyle& style,
                     pg::FilamentPath* record) {
    pg::FilamentPath path;
    path.add_line((float)x0, (float)y, (float)x1, (float)y);
    draw_lane(layer, path, style, record);
}

} // namespace ui
} // namespace helix

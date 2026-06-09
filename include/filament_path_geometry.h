// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_path_geometry.h
 * @brief Pure-geometry foundation for AMS filament-path rendering.
 *
 * Orthogonal lane routing: straight vertical runs joined by true quarter-circle
 * arc fillets. This module is intentionally LVGL-free (plain floats) so it can
 * be unit-tested headlessly; the canvas renderer (filament_tube_stroker)
 * consumes the resulting ARC segments by walking their exact float
 * parametrization as a fan of short straight chords (lv_draw_line), so arc and
 * line bands stay flush at every joint.
 *
 * Coordinate system: SCREEN coordinates, +y is DOWN.
 *   - Angle 0 = +x axis.
 *   - Positive angles rotate toward +y (standard atan2 in screen space).
 *   - Positive sweep = CLOCKWISE on screen.
 */

namespace helix {
namespace ui {
namespace pathgeo {

struct PathPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct PathSeg {
    enum Type { LINE, ARC };
    Type type = LINE;

    // LINE: p0 -> p1
    PathPoint p0;
    PathPoint p1;

    // ARC: circle center, centerline radius, start angle, signed sweep (radians).
    // A point on the arc at parameter t in [0, |sweep|] is:
    //   angle = start_angle + sign(sweep) * t
    //   (cx + r*cos(angle), cy + r*sin(angle))
    PathPoint center;
    float radius = 0.0f;
    float start_angle = 0.0f;
    float sweep = 0.0f;
};

struct FilamentPath {
    static constexpr int MAX_SEGS = 16;

    PathSeg segs[MAX_SEGS];
    int count = 0;

    /// Append a straight segment. Zero-length lines are skipped.
    void add_line(float x0, float y0, float x1, float y1);

    /// Append an arc segment (center, centerline radius, start angle, signed sweep).
    void add_arc(float cx, float cy, float r, float a0, float sweep);

    void clear();
};

/// LINE: euclidean distance. ARC: |sweep| * radius.
float seg_length(const PathSeg& s);

/// Sum of all segment lengths.
float path_length(const FilamentPath& p);

/**
 * @brief Point at arc-length distance @p d along the whole path.
 *
 * @p d is clamped to [0, total_length]. If @p tangent_out is non-null it
 * receives the unit tangent direction (pointing in the direction of increasing
 * arc length) at that point.
 */
PathPoint path_point_at(const FilamentPath& p, float d, PathPoint* tangent_out = nullptr);

/**
 * @brief Orthogonal lane routing from (x0,y0) DOWN to (x1,y1), y1 > y0.
 *
 * Produces: vertical drop -> quarter-arc fillet -> horizontal run at mid-height
 * -> quarter-arc fillet -> vertical drop into (x1,y1). Segments are appended to
 * @p out (it is NOT cleared first).
 *
 * @p fillet_r is the desired centerline fillet radius; the effective radius is
 * clamped to r_eff = min(fillet_r, |dx|/2, dy/2).
 *
 * Degenerate cases:
 *   - y1 <= y0     -> single straight LINE p0->p1 (defensive fallback)
 *   - |dx| < 2.0   -> single vertical LINE
 *   - r_eff < 2.0  -> 45-degree jog: vertical, diagonal LINE, vertical (no arcs)
 */
void route_orthogonal(FilamentPath& out, float x0, float y0, float x1, float y1, float fillet_r);

/**
 * @brief General filleted-polyline routing through arbitrary waypoints.
 *
 * Appends LINE segments through @p pts (n points) with a tangent circular-arc
 * fillet inserted at each interior vertex. Unlike route_orthogonal this does NOT
 * assume perpendicular bends — corners between non-orthogonal segments are
 * filleted with standard corner-fillet math. Segments are appended to @p out (it
 * is NOT cleared first).
 *
 * At interior vertex V with incoming unit direction d1 and outgoing unit
 * direction d2:
 *   - interior half-angle alpha = half the angle between -d1 and d2,
 *   - tangent trim back along each leg t = r_eff / tan(alpha),
 *   - arc center sits r_eff / sin(alpha) from V along the inward bisector,
 *   - sweep magnitude = pi - 2*alpha, sign from cross(d1, d2) in screen coords
 *     (+y down): positive cross => clockwise on screen => positive sweep
 *     (matching the ARC convention so path_point_at walks it forward).
 *
 * Per-vertex r_eff is clamped so the trim never exceeds half of either adjoining
 * segment length. Collinear vertices (cross ~ 0) get no fillet (the line simply
 * continues); duplicate / degenerate-short legs fall back to plain lines.
 *
 * Degenerate cases:
 *   - n < 2          -> nothing appended
 *   - n == 2         -> single straight LINE pts[0]->pts[1]
 *
 * @p fillet_r is the desired centerline fillet radius (clamped per vertex).
 */
void route_polyline_filleted(FilamentPath& out, const PathPoint* pts, int n, float fillet_r);

} // namespace pathgeo
} // namespace ui
} // namespace helix

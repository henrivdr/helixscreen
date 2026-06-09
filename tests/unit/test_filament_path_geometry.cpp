// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_path_geometry.h"

#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::pathgeo;

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dist(const PathPoint& a, const PathPoint& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Tangent of the LAST segment at its endpoint (where it meets the next seg),
// pointing forward (direction of increasing arc length).
PathPoint seg_end_tangent(const PathSeg& s) {
    if (s.type == PathSeg::LINE) {
        float dx = s.p1.x - s.p0.x;
        float dy = s.p1.y - s.p0.y;
        float len = std::sqrt(dx * dx + dy * dy);
        return {dx / len, dy / len};
    }
    // ARC: tangent at end angle, in direction of sweep.
    float end_angle = s.start_angle + s.sweep;
    float sgn = (s.sweep >= 0.0f) ? 1.0f : -1.0f;
    // d/dt (cos, sin) = (-sin, cos); multiply by sign of sweep for direction.
    return {-std::sin(end_angle) * sgn, std::cos(end_angle) * sgn};
}

PathPoint seg_start_tangent(const PathSeg& s) {
    if (s.type == PathSeg::LINE) {
        float dx = s.p1.x - s.p0.x;
        float dy = s.p1.y - s.p0.y;
        float len = std::sqrt(dx * dx + dy * dy);
        return {dx / len, dy / len};
    }
    float sgn = (s.sweep >= 0.0f) ? 1.0f : -1.0f;
    return {-std::sin(s.start_angle) * sgn, std::cos(s.start_angle) * sgn};
}

PathPoint seg_start_point(const PathSeg& s) {
    if (s.type == PathSeg::LINE)
        return s.p0;
    return {s.center.x + s.radius * std::cos(s.start_angle),
            s.center.y + s.radius * std::sin(s.start_angle)};
}

PathPoint seg_end_point(const PathSeg& s) {
    if (s.type == PathSeg::LINE)
        return s.p1;
    float a = s.start_angle + s.sweep;
    return {s.center.x + s.radius * std::cos(a), s.center.y + s.radius * std::sin(a)};
}

} // namespace

// ============================================================================
// seg_length / path_length
// ============================================================================

TEST_CASE("seg_length LINE is euclidean", "[filament-path][geometry]") {
    PathSeg s;
    s.type = PathSeg::LINE;
    s.p0 = {0.0f, 0.0f};
    s.p1 = {3.0f, 4.0f};
    REQUIRE(seg_length(s) == Catch::Approx(5.0f));
}

TEST_CASE("seg_length ARC is |sweep|*radius", "[filament-path][geometry]") {
    PathSeg s;
    s.type = PathSeg::ARC;
    s.center = {0.0f, 0.0f};
    s.radius = 10.0f;
    s.start_angle = 0.0f;
    s.sweep = kPi / 2.0f; // quarter circle
    REQUIRE(seg_length(s) == Catch::Approx(10.0f * kPi / 2.0f));

    // Negative sweep -> same length (uses |sweep|).
    s.sweep = -kPi / 2.0f;
    REQUIRE(seg_length(s) == Catch::Approx(10.0f * kPi / 2.0f));
}

TEST_CASE("path_length sums mixed segments", "[filament-path][geometry]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 10.0f);           // length 10
    p.add_arc(5.0f, 10.0f, 5.0f, kPi, kPi / 2.0f); // quarter arc r=5 -> 5*pi/2
    p.add_line(5.0f, 15.0f, 15.0f, 15.0f);         // length 10
    float expect = 10.0f + 5.0f * kPi / 2.0f + 10.0f;
    REQUIRE(path_length(p) == Catch::Approx(expect));
}

TEST_CASE("add_line skips zero-length, add ops respect MAX_SEGS", "[filament-path][geometry]") {
    FilamentPath p;
    p.add_line(1.0f, 1.0f, 1.0f, 1.0f); // zero length -> skipped
    REQUIRE(p.count == 0);

    p.clear();
    for (int i = 0; i < FilamentPath::MAX_SEGS + 5; ++i) {
        p.add_line(0.0f, static_cast<float>(i), 0.0f, static_cast<float>(i + 1));
    }
    REQUIRE(p.count == FilamentPath::MAX_SEGS);
}

// ============================================================================
// path_point_at
// ============================================================================

TEST_CASE("path_point_at endpoints and clamping on a single line", "[filament-path][geometry]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 100.0f);

    PathPoint at0 = path_point_at(p, 0.0f);
    REQUIRE(at0.x == Catch::Approx(0.0f));
    REQUIRE(at0.y == Catch::Approx(0.0f));

    PathPoint atEnd = path_point_at(p, 100.0f);
    REQUIRE(atEnd.y == Catch::Approx(100.0f));

    PathPoint mid = path_point_at(p, 50.0f);
    REQUIRE(mid.y == Catch::Approx(50.0f));

    // clamp below 0
    PathPoint clampLow = path_point_at(p, -20.0f);
    REQUIRE(clampLow.y == Catch::Approx(0.0f));

    // clamp beyond total
    PathPoint clampHigh = path_point_at(p, 500.0f);
    REQUIRE(clampHigh.y == Catch::Approx(100.0f));
}

TEST_CASE("path_point_at tangent on vertical line points down", "[filament-path][geometry]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 100.0f);
    PathPoint tangent;
    path_point_at(p, 50.0f, &tangent);
    REQUIRE(tangent.x == Catch::Approx(0.0f).margin(1e-4));
    REQUIRE(tangent.y == Catch::Approx(1.0f).margin(1e-4)); // +y down
}

TEST_CASE("path_point_at on arc midpoint and tangent", "[filament-path][geometry]") {
    // Quarter arc: center (0,0), r=10, start angle 0 (point (10,0)),
    // sweep +pi/2 (clockwise on screen) -> ends at (0,10).
    FilamentPath p;
    p.add_arc(0.0f, 0.0f, 10.0f, 0.0f, kPi / 2.0f);

    float total = path_length(p);
    REQUIRE(total == Catch::Approx(10.0f * kPi / 2.0f));

    // Midpoint at d = total/2 -> angle pi/4 -> (10*cos45, 10*sin45).
    PathPoint tangent;
    PathPoint midp = path_point_at(p, total / 2.0f, &tangent);
    REQUIRE(midp.x == Catch::Approx(10.0f * std::cos(kPi / 4.0f)));
    REQUIRE(midp.y == Catch::Approx(10.0f * std::sin(kPi / 4.0f)));

    // Unit tangent.
    float tlen = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
    REQUIRE(tlen == Catch::Approx(1.0f).margin(1e-4));
    // At pi/4, moving clockwise (+sweep): direction = (-sin, cos) = (-0.707, 0.707).
    REQUIRE(tangent.x == Catch::Approx(-std::sin(kPi / 4.0f)).margin(1e-3));
    REQUIRE(tangent.y == Catch::Approx(std::cos(kPi / 4.0f)).margin(1e-3));
}

TEST_CASE("path_point_at exactly on segment boundary", "[filament-path][geometry]") {
    FilamentPath p;
    p.add_line(0.0f, 0.0f, 0.0f, 10.0f);
    p.add_line(0.0f, 10.0f, 10.0f, 10.0f);

    // boundary at d=10 should be the shared corner.
    PathPoint corner = path_point_at(p, 10.0f);
    REQUIRE(corner.x == Catch::Approx(0.0f).margin(1e-4));
    REQUIRE(corner.y == Catch::Approx(10.0f).margin(1e-4));

    // just past boundary -> moving along horizontal.
    PathPoint past = path_point_at(p, 13.0f);
    REQUIRE(past.x == Catch::Approx(3.0f));
    REQUIRE(past.y == Catch::Approx(10.0f));
}

// ============================================================================
// route_orthogonal — tangent continuity
// ============================================================================

// Endpoint chaining only (positions match end-to-end). Used for paths whose
// joints are intentionally corners (the 45-degree jog).
static void check_positions(const FilamentPath& p, float x0, float y0, float x1, float y1) {
    REQUIRE(p.count >= 1);
    PathPoint start = seg_start_point(p.segs[0]);
    PathPoint end = seg_end_point(p.segs[p.count - 1]);
    REQUIRE(start.x == Catch::Approx(x0).margin(1e-3));
    REQUIRE(start.y == Catch::Approx(y0).margin(1e-3));
    REQUIRE(end.x == Catch::Approx(x1).margin(1e-3));
    REQUIRE(end.y == Catch::Approx(y1).margin(1e-3));
    for (int i = 0; i + 1 < p.count; ++i) {
        PathPoint prevEnd = seg_end_point(p.segs[i]);
        PathPoint nextStart = seg_start_point(p.segs[i + 1]);
        INFO("joint " << i << " positions");
        REQUIRE(dist(prevEnd, nextStart) < 1e-3f);
    }
}

static void check_continuity_and_endpoints(const FilamentPath& p, float x0, float y0, float x1,
                                           float y1) {
    REQUIRE(p.count >= 1);

    // Path starts at (x0,y0) and ends at (x1,y1).
    PathPoint start = seg_start_point(p.segs[0]);
    PathPoint end = seg_end_point(p.segs[p.count - 1]);
    REQUIRE(start.x == Catch::Approx(x0).margin(1e-3));
    REQUIRE(start.y == Catch::Approx(y0).margin(1e-3));
    REQUIRE(end.x == Catch::Approx(x1).margin(1e-3));
    REQUIRE(end.y == Catch::Approx(y1).margin(1e-3));

    // Each joint: prev endpoint == next start point, prev end tangent == next start tangent.
    for (int i = 0; i + 1 < p.count; ++i) {
        PathPoint prevEnd = seg_end_point(p.segs[i]);
        PathPoint nextStart = seg_start_point(p.segs[i + 1]);
        INFO("joint " << i << " positions");
        REQUIRE(dist(prevEnd, nextStart) < 1e-3f);

        PathPoint prevT = seg_end_tangent(p.segs[i]);
        PathPoint nextT = seg_start_tangent(p.segs[i + 1]);
        INFO("joint " << i << " tangents: prev(" << prevT.x << "," << prevT.y << ") next("
                      << nextT.x << "," << nextT.y << ")");
        REQUIRE(prevT.x == Catch::Approx(nextT.x).margin(1e-3));
        REQUIRE(prevT.y == Catch::Approx(nextT.y).margin(1e-3));
    }
}

TEST_CASE("route_orthogonal dx>0 full S with arcs", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 0.0f, 0.0f, 100.0f, 200.0f, 20.0f);

    // vertical, arc, horizontal, arc, vertical
    REQUIRE(p.count == 5);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
    REQUIRE(p.segs[1].type == PathSeg::ARC);
    REQUIRE(p.segs[2].type == PathSeg::LINE);
    REQUIRE(p.segs[3].type == PathSeg::ARC);
    REQUIRE(p.segs[4].type == PathSeg::LINE);

    check_continuity_and_endpoints(p, 0.0f, 0.0f, 100.0f, 200.0f);

    // Horizontal run is at ymid.
    float ymid = 0.0f + (200.0f - 0.0f) / 2.0f;
    REQUIRE(p.segs[2].p0.y == Catch::Approx(ymid).margin(1e-3));
    REQUIRE(p.segs[2].p1.y == Catch::Approx(ymid).margin(1e-3));
}

TEST_CASE("route_orthogonal dx<0 full S with arcs", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 100.0f, 0.0f, 0.0f, 200.0f, 20.0f);

    REQUIRE(p.count == 5);
    check_continuity_and_endpoints(p, 100.0f, 0.0f, 0.0f, 200.0f);

    float ymid = 100.0f;
    REQUIRE(p.segs[2].p0.y == Catch::Approx(ymid).margin(1e-3));
}

TEST_CASE("route_orthogonal total length is analytic", "[filament-path][geometry]") {
    float x0 = 0, y0 = 0, x1 = 80, y1 = 200, r = 15;
    FilamentPath p;
    route_orthogonal(p, x0, y0, x1, y1, r);

    float dx = x1 - x0, dy = y1 - y0;
    float r_eff = std::min({r, std::fabs(dx) / 2.0f, dy / 2.0f});
    // verticals + horizontal + 2 quarter arcs.
    // vertical total = dy - 2*r_eff ; horizontal = |dx| - 2*r_eff ; arcs = 2*(pi/2)*r_eff
    float expect =
        (dy - 2.0f * r_eff) + (std::fabs(dx) - 2.0f * r_eff) + 2.0f * (kPi / 2.0f) * r_eff;
    REQUIRE(path_length(p) == Catch::Approx(expect).margin(1e-2));
}

// ============================================================================
// route_orthogonal — clamping & degenerate cases
// ============================================================================

TEST_CASE("route_orthogonal large fillet clamped to dx/2", "[filament-path][geometry]") {
    // |dx|=40 -> r_eff capped at 20; dy=200 plenty. fillet_r=1000 huge.
    // With r==|dx|/2 the horizontal run is zero-length and is dropped, so the
    // path is vertical, arc, arc, vertical (4 segs). The fillet radius is what
    // matters: it must be clamped to 20.
    FilamentPath p;
    route_orthogonal(p, 0.0f, 0.0f, 40.0f, 200.0f, 1000.0f);
    bool found_arc = false;
    for (int i = 0; i < p.count; ++i) {
        if (p.segs[i].type == PathSeg::ARC) {
            found_arc = true;
            REQUIRE(p.segs[i].radius == Catch::Approx(20.0f).margin(1e-3));
        }
    }
    REQUIRE(found_arc);
    check_continuity_and_endpoints(p, 0.0f, 0.0f, 40.0f, 200.0f);
}

TEST_CASE("route_orthogonal large fillet clamped to dy/2", "[filament-path][geometry]") {
    // dy=30 -> r_eff capped at 15; dx=400 plenty. With r==dy/2 both verticals
    // are zero-length and dropped, leaving arc, horizontal, arc (3 segs).
    FilamentPath p;
    route_orthogonal(p, 0.0f, 0.0f, 400.0f, 30.0f, 1000.0f);
    bool found_arc = false;
    for (int i = 0; i < p.count; ++i) {
        if (p.segs[i].type == PathSeg::ARC) {
            found_arc = true;
            REQUIRE(p.segs[i].radius == Catch::Approx(15.0f).margin(1e-3));
        }
    }
    REQUIRE(found_arc);
    check_continuity_and_endpoints(p, 0.0f, 0.0f, 400.0f, 30.0f);
}

TEST_CASE("route_orthogonal narrow dx -> single vertical", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 10.0f, 0.0f, 11.0f, 200.0f, 20.0f); // dx=1 < 2
    REQUIRE(p.count == 1);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
    REQUIRE(p.segs[0].p0.x == Catch::Approx(10.0f));
    REQUIRE(p.segs[0].p1.x == Catch::Approx(11.0f));
    REQUIRE(p.segs[0].p1.y == Catch::Approx(200.0f));
}

TEST_CASE("route_orthogonal tiny r_eff -> 45 degree jog (no arcs)", "[filament-path][geometry]") {
    // dx large enough (>=2) but dy tiny so r_eff = dy/2 < 2.
    // dy=2 -> r_eff = 1 < 2 -> jog.
    FilamentPath p;
    route_orthogonal(p, 0.0f, 0.0f, 50.0f, 2.0f, 20.0f);
    REQUIRE(p.count == 3);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
    REQUIRE(p.segs[1].type == PathSeg::LINE); // diagonal
    REQUIRE(p.segs[2].type == PathSeg::LINE);
    // No arcs anywhere.
    for (int i = 0; i < p.count; ++i)
        REQUIRE(p.segs[i].type == PathSeg::LINE);
    check_positions(p, 0.0f, 0.0f, 50.0f, 2.0f);
}

TEST_CASE("route_orthogonal y1<=y0 fallback straight line", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 0.0f, 100.0f, 50.0f, 100.0f, 20.0f); // y1==y0
    REQUIRE(p.count == 1);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
    REQUIRE(p.segs[0].p0.x == Catch::Approx(0.0f));
    REQUIRE(p.segs[0].p1.x == Catch::Approx(50.0f));

    FilamentPath p2;
    route_orthogonal(p2, 0.0f, 100.0f, 50.0f, 50.0f, 20.0f); // y1<y0
    REQUIRE(p2.count == 1);
    REQUIRE(p2.segs[0].type == PathSeg::LINE);
}

// ============================================================================
// End-to-end walk: monotonic progress, no jumps (catches angle-direction bugs)
// ============================================================================

TEST_CASE("route_orthogonal walk is smooth and monotonic (dx>0)", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 0.0f, 0.0f, 120.0f, 240.0f, 25.0f);
    float total = path_length(p);

    PathPoint prev = path_point_at(p, 0.0f);
    float prev_y = prev.y;
    for (float d = 2.0f; d <= total; d += 2.0f) {
        PathPoint cur = path_point_at(p, d);
        float step = dist(prev, cur);
        INFO("d=" << d << " step=" << step << " cur(" << cur.x << "," << cur.y << ")");
        REQUIRE(step < 3.0f);             // no large jumps
        REQUIRE(cur.y >= prev_y - 1e-3f); // progress downward is monotonic
        prev = cur;
        prev_y = cur.y;
    }
}

TEST_CASE("route_orthogonal walk is smooth and monotonic (dx<0)", "[filament-path][geometry]") {
    FilamentPath p;
    route_orthogonal(p, 120.0f, 0.0f, 0.0f, 240.0f, 25.0f);
    float total = path_length(p);

    PathPoint prev = path_point_at(p, 0.0f);
    float prev_y = prev.y;
    for (float d = 2.0f; d <= total; d += 2.0f) {
        PathPoint cur = path_point_at(p, d);
        float step = dist(prev, cur);
        INFO("d=" << d << " step=" << step << " cur(" << cur.x << "," << cur.y << ")");
        REQUIRE(step < 3.0f);
        REQUIRE(cur.y >= prev_y - 1e-3f);
        prev = cur;
        prev_y = cur.y;
    }
}

// ============================================================================
// route_polyline_filleted — general filleted-polyline routing
// ============================================================================

// Analytic length of a filleted polyline: sum of trimmed straight runs plus the
// arc lengths. We don't predict it; we cross-check path_length against a dense
// walk instead. These helpers verify tangency at each interior fillet.

// At every LINE->ARC and ARC->LINE joint, endpoints must coincide and tangents
// must match. route_polyline_filleted produces alternating LINE/ARC/LINE/...,
// so the generic continuity checker applies directly.

TEST_CASE("route_polyline_filleted 45-degree corner: tangency + sweep sign",
          "[filament-path][geometry]") {
    // Right turn: go right, then down-right at 45 degrees.
    // p0=(0,0) -> V=(100,0) -> p2=(150,50). Interior turn is a clockwise bend
    // on screen (heading +x then +x+y) => positive sweep.
    PathPoint pts[3] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {150.0f, 50.0f}};
    FilamentPath p;
    route_polyline_filleted(p, pts, 3, 15.0f);

    // LINE, ARC, LINE.
    REQUIRE(p.count == 3);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
    REQUIRE(p.segs[1].type == PathSeg::ARC);
    REQUIRE(p.segs[2].type == PathSeg::LINE);

    // Clockwise turn on screen (+x -> down-right) => positive sweep.
    REQUIRE(p.segs[1].sweep > 0.0f);

    check_continuity_and_endpoints(p, 0.0f, 0.0f, 150.0f, 50.0f);
}

TEST_CASE("route_polyline_filleted left turn yields negative sweep", "[filament-path][geometry]") {
    // Heading +x then up-right (+x,-y): counter-clockwise on screen => negative sweep.
    PathPoint pts[3] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {150.0f, -50.0f}};
    FilamentPath p;
    route_polyline_filleted(p, pts, 3, 15.0f);
    REQUIRE(p.count == 3);
    REQUIRE(p.segs[1].type == PathSeg::ARC);
    REQUIRE(p.segs[1].sweep < 0.0f);
    check_continuity_and_endpoints(p, 0.0f, 0.0f, 150.0f, -50.0f);
}

TEST_CASE("route_polyline_filleted shallow (20-degree) corner: tangency",
          "[filament-path][geometry]") {
    // Shallow bend: nearly collinear, ~20 degrees of turn.
    // First leg along +x; second leg turned 20 degrees toward +y.
    float ang = 20.0f * kPi / 180.0f;
    PathPoint pts[3] = {
        {0.0f, 0.0f},
        {200.0f, 0.0f},
        {200.0f + 200.0f * std::cos(ang), 0.0f + 200.0f * std::sin(ang)},
    };
    FilamentPath p;
    route_polyline_filleted(p, pts, 3, 10.0f);
    REQUIRE(p.count == 3);
    REQUIRE(p.segs[1].type == PathSeg::ARC);
    REQUIRE(p.segs[1].sweep > 0.0f);
    check_continuity_and_endpoints(p, pts[0].x, pts[0].y, pts[2].x, pts[2].y);
}

TEST_CASE("route_polyline_filleted multi-vertex mixed turns: dense walk",
          "[filament-path][geometry]") {
    // 5 waypoints, alternating turn directions.
    PathPoint pts[5] = {
        {0.0f, 0.0f}, {80.0f, 20.0f}, {160.0f, 0.0f}, {220.0f, 60.0f}, {300.0f, 40.0f},
    };
    FilamentPath p;
    route_polyline_filleted(p, pts, 5, 12.0f);

    // Endpoints and per-joint tangency.
    check_continuity_and_endpoints(p, pts[0].x, pts[0].y, pts[4].x, pts[4].y);

    float total = path_length(p);

    // Dense walk: 2px steps, no jump > 3px.
    PathPoint prev = path_point_at(p, 0.0f);
    float walk_len = 0.0f;
    for (float d = 2.0f; d <= total; d += 2.0f) {
        PathPoint cur = path_point_at(p, d);
        float step = dist(prev, cur);
        INFO("d=" << d << " step=" << step << " cur(" << cur.x << "," << cur.y << ")");
        REQUIRE(step < 3.0f);
        walk_len += step;
        prev = cur;
    }
    // The dense walk underestimates the true length: chord-vs-arc deficit on the
    // tight fillets plus the untraversed tail (loop stops at the last d <= total,
    // leaving up to one 2px step). Both shortfalls are bounded; the analytic sum
    // is path_length(total) itself. Require the walk to be within ~one step.
    REQUIRE(walk_len <= total + 1e-3f);
    REQUIRE(walk_len == Catch::Approx(total).margin(2.5f));
}

TEST_CASE("route_polyline_filleted oversized r clamped on short middle segment",
          "[filament-path][geometry]") {
    // Middle segment is short (length 20); huge fillet must clamp so trims never
    // exceed half the adjoining segment lengths and joints stay coincident.
    PathPoint pts[4] = {
        {0.0f, 0.0f},
        {100.0f, 0.0f},
        {120.0f, 0.0f},
        {120.0f, 100.0f},
    };
    // Note middle vertices (100,0) collinear with first leg; the real corner is
    // at (120,0). Use a layout with an actual short middle leg instead:
    PathPoint q[4] = {
        {0.0f, 0.0f},
        {100.0f, 0.0f},
        {110.0f, 20.0f},
        {110.0f, 120.0f},
    };
    FilamentPath p;
    route_polyline_filleted(p, q, 4, 1000.0f);
    // Must not crash, endpoints preserved, all joints coincident.
    check_continuity_and_endpoints(p, q[0].x, q[0].y, q[3].x, q[3].y);
    // Every arc radius must be positive and finite.
    for (int i = 0; i < p.count; ++i) {
        if (p.segs[i].type == PathSeg::ARC) {
            REQUIRE(p.segs[i].radius > 0.0f);
            REQUIRE(std::isfinite(p.segs[i].radius));
        }
    }
    LV_UNUSED(pts);
}

TEST_CASE("route_polyline_filleted collinear waypoint gets no fillet",
          "[filament-path][geometry]") {
    // Middle point is exactly on the line p0->p2: no arc, just a straight run.
    PathPoint pts[3] = {{0.0f, 0.0f}, {50.0f, 0.0f}, {100.0f, 0.0f}};
    FilamentPath p;
    route_polyline_filleted(p, pts, 3, 15.0f);
    for (int i = 0; i < p.count; ++i)
        REQUIRE(p.segs[i].type == PathSeg::LINE);
    // End-to-end span preserved.
    PathPoint start = seg_start_point(p.segs[0]);
    PathPoint end = seg_end_point(p.segs[p.count - 1]);
    REQUIRE(start.x == Catch::Approx(0.0f).margin(1e-3));
    REQUIRE(end.x == Catch::Approx(100.0f).margin(1e-3));
}

TEST_CASE("route_polyline_filleted duplicate waypoint is skipped", "[filament-path][geometry]") {
    // Duplicate middle point: degenerate zero-length leg must not produce NaNs.
    PathPoint pts[4] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}};
    FilamentPath p;
    route_polyline_filleted(p, pts, 4, 15.0f);
    check_continuity_and_endpoints(p, 0.0f, 0.0f, 100.0f, 100.0f);
    for (int i = 0; i < p.count; ++i) {
        if (p.segs[i].type == PathSeg::ARC) {
            REQUIRE(std::isfinite(p.segs[i].radius));
            REQUIRE(std::isfinite(p.segs[i].sweep));
        }
    }
}

TEST_CASE("route_polyline_filleted two-point degenerate is a single line",
          "[filament-path][geometry]") {
    PathPoint pts[2] = {{0.0f, 0.0f}, {100.0f, 50.0f}};
    FilamentPath p;
    route_polyline_filleted(p, pts, 2, 15.0f);
    REQUIRE(p.count == 1);
    REQUIRE(p.segs[0].type == PathSeg::LINE);
}

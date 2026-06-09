// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_path_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix {
namespace ui {
namespace pathgeo {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi / 2.0f;

// Minimum horizontal offset before we bother routing around a corner.
constexpr float kMinDx = 2.0f;
// Minimum effective fillet radius before arcs are worthwhile.
constexpr float kMinFillet = 2.0f;
} // namespace

void FilamentPath::add_line(float x0, float y0, float x1, float y1) {
    if (count >= MAX_SEGS)
        return;
    // Skip zero-length lines.
    float dx = x1 - x0;
    float dy = y1 - y0;
    if (dx * dx + dy * dy <= 0.0f)
        return;
    PathSeg& s = segs[count++];
    s.type = PathSeg::LINE;
    s.p0 = {x0, y0};
    s.p1 = {x1, y1};
    s.center = {0.0f, 0.0f};
    s.radius = 0.0f;
    s.start_angle = 0.0f;
    s.sweep = 0.0f;
}

void FilamentPath::add_arc(float cx, float cy, float r, float a0, float sweep) {
    if (count >= MAX_SEGS)
        return;
    PathSeg& s = segs[count++];
    s.type = PathSeg::ARC;
    s.center = {cx, cy};
    s.radius = r;
    s.start_angle = a0;
    s.sweep = sweep;
    // p0/p1 left as defaults for ARC; consumers use center/angle.
    s.p0 = {cx + r * std::cos(a0), cy + r * std::sin(a0)};
    s.p1 = {cx + r * std::cos(a0 + sweep), cy + r * std::sin(a0 + sweep)};
}

void FilamentPath::clear() {
    count = 0;
}

float seg_length(const PathSeg& s) {
    if (s.type == PathSeg::LINE) {
        float dx = s.p1.x - s.p0.x;
        float dy = s.p1.y - s.p0.y;
        return std::sqrt(dx * dx + dy * dy);
    }
    return std::fabs(s.sweep) * s.radius;
}

float path_length(const FilamentPath& p) {
    float total = 0.0f;
    for (int i = 0; i < p.count; ++i)
        total += seg_length(p.segs[i]);
    return total;
}

PathPoint path_point_at(const FilamentPath& p, float d, PathPoint* tangent_out) {
    if (p.count <= 0) {
        if (tangent_out)
            *tangent_out = {0.0f, 0.0f};
        return {0.0f, 0.0f};
    }

    float total = path_length(p);
    if (d < 0.0f)
        d = 0.0f;
    if (d > total)
        d = total;

    float accum = 0.0f;
    for (int i = 0; i < p.count; ++i) {
        const PathSeg& s = p.segs[i];
        float len = seg_length(s);
        // Use the last segment for the trailing boundary so d == total resolves
        // to the final endpoint rather than falling through.
        bool last = (i == p.count - 1);
        if (d <= accum + len || last) {
            float into = d - accum;
            if (into < 0.0f)
                into = 0.0f;
            if (into > len)
                into = len;

            if (s.type == PathSeg::LINE) {
                float t = (len > 0.0f) ? (into / len) : 0.0f;
                PathPoint pt{s.p0.x + (s.p1.x - s.p0.x) * t, s.p0.y + (s.p1.y - s.p0.y) * t};
                if (tangent_out) {
                    float dx = s.p1.x - s.p0.x;
                    float dy = s.p1.y - s.p0.y;
                    float l = std::sqrt(dx * dx + dy * dy);
                    *tangent_out = (l > 0.0f) ? PathPoint{dx / l, dy / l} : PathPoint{0.0f, 0.0f};
                }
                return pt;
            }

            // ARC: angle advances at unit rate per centerline arc length.
            float sgn = (s.sweep >= 0.0f) ? 1.0f : -1.0f;
            float angle = s.start_angle + sgn * (s.radius > 0.0f ? into / s.radius : 0.0f);
            PathPoint pt{s.center.x + s.radius * std::cos(angle),
                         s.center.y + s.radius * std::sin(angle)};
            if (tangent_out) {
                // Forward tangent = d/dt of (cos,sin) scaled by sweep sign, unit length.
                *tangent_out = {-std::sin(angle) * sgn, std::cos(angle) * sgn};
            }
            return pt;
        }
        accum += len;
    }

    // Unreachable (last-segment branch handles d == total), but be defensive.
    const PathSeg& s = p.segs[p.count - 1];
    if (tangent_out)
        *tangent_out = {0.0f, 0.0f};
    return (s.type == PathSeg::LINE) ? s.p1 : s.p1;
}

void route_orthogonal(FilamentPath& out, float x0, float y0, float x1, float y1, float fillet_r) {
    float dx = x1 - x0;
    float dy = y1 - y0;

    // Defensive fallback: not strictly going down.
    if (dy <= 0.0f) {
        out.add_line(x0, y0, x1, y1);
        return;
    }

    // Negligible horizontal offset: a single vertical run.
    if (std::fabs(dx) < kMinDx) {
        out.add_line(x0, y0, x1, y1);
        return;
    }

    float dir = (dx > 0.0f) ? 1.0f : -1.0f;
    float r_eff = std::min({fillet_r, std::fabs(dx) / 2.0f, dy / 2.0f});

    // Fillet too small to draw meaningful arcs: 45-degree jog (3 straight lines).
    if (r_eff < kMinFillet) {
        float y_a = y0 + dy / 3.0f;
        float y_b = y0 + 2.0f * dy / 3.0f;
        out.add_line(x0, y0, x0, y_a);  // vertical
        out.add_line(x0, y_a, x1, y_b); // diagonal
        out.add_line(x1, y_b, x1, y1);  // vertical
        return;
    }

    float ymid = y0 + dy / 2.0f;
    float r = r_eff;

    // Seg 0: vertical drop to the top of the first fillet.
    out.add_line(x0, y0, x0, ymid - r);

    // Arc 1: vertical -> horizontal fillet.
    // Center sits one radius horizontally toward the target and one radius up
    // from the mid-height line. Start angle / sweep chosen so the tangent leaves
    // downward and arrives horizontal toward the target.
    {
        float cx = x0 + dir * r;
        float cy = ymid - r;
        float a0 = (dir > 0.0f) ? kPi : 0.0f;
        float sweep = -dir * kHalfPi;
        out.add_arc(cx, cy, r, a0, sweep);
    }

    // Seg 2: horizontal run at mid-height.
    out.add_line(x0 + dir * r, ymid, x1 - dir * r, ymid);

    // Arc 2: horizontal -> vertical fillet into the target.
    {
        float cx = x1 - dir * r;
        float cy = ymid + r;
        float a0 = -kHalfPi;
        float sweep = dir * kHalfPi;
        out.add_arc(cx, cy, r, a0, sweep);
    }

    // Seg 4: vertical drop into the target.
    out.add_line(x1, ymid + r, x1, y1);
}

} // namespace pathgeo
} // namespace ui
} // namespace helix

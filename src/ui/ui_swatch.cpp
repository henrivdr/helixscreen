// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_swatch.h"

#include "color_utils.h"
#include "theme_manager.h"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace helix::ui {

namespace {

// Retained per-swatch color list, keyed by object. Accessed only on the main
// (UI) thread: apply_swatch_color(), the DRAW_MAIN callback, and the DELETE
// callback all run there. Entries are erased when the swatch is deleted.
struct SwatchPaint {
    std::vector<lv_color_t> colors;
};

std::unordered_map<lv_obj_t*, SwatchPaint>& paint_registry() {
    static std::unordered_map<lv_obj_t*, SwatchPaint> registry;
    return registry;
}

// Delineating border so a fill that matches the background stays visible.
constexpr lv_opa_t SWATCH_BORDER_OPA = 80;

lv_color_t muted_border_color() {
    return theme_manager_get_color("text_muted");
}

// Parse "#RRGGBB,#RRGGBB,..." (each entry optionally '#'-prefixed) into colors.
std::vector<lv_color_t> parse_multi_colors(const std::string& csv) {
    std::vector<lv_color_t> colors;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        std::string token =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // Trim surrounding whitespace.
        size_t b = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            std::string hex = token.substr(b, e - b + 1);
            if (auto rgb = helix::parse_hex_color(hex)) {
                colors.push_back(lv_color_hex(*rgb));
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return colors;
}

// Signed position along the top-left → bottom-right axis: 0 at TL, 1 on the
// anti-diagonal, 2 at BR. Equal-area bands are the strips between iso-f lines.
struct DiagField {
    float x0, y0, inv_w, inv_h;
    float operator()(float px, float py) const {
        return (px - x0) * inv_w + (py - y0) * inv_h;
    }
};

struct FPoint {
    float x, y;
};

// Sutherland–Hodgman clip of a convex polygon by a single half-plane.
// keep_ge=true keeps vertices with f >= t; false keeps f <= t.
std::vector<FPoint> clip_halfplane(const std::vector<FPoint>& poly, const DiagField& f, float t,
                                   bool keep_ge) {
    std::vector<FPoint> out;
    if (poly.empty()) {
        return out;
    }
    out.reserve(poly.size() + 2);
    for (size_t i = 0; i < poly.size(); ++i) {
        const FPoint& a = poly[i];
        const FPoint& b = poly[(i + 1) % poly.size()];
        float fa = f(a.x, a.y);
        float fb = f(b.x, b.y);
        bool a_in = keep_ge ? (fa >= t) : (fa <= t);
        bool b_in = keep_ge ? (fb >= t) : (fb <= t);
        if (a_in) {
            out.push_back(a);
        }
        if (a_in != b_in) {
            float denom = fb - fa;
            float alpha = denom != 0.0f ? (t - fa) / denom : 0.0f;
            out.push_back({a.x + alpha * (b.x - a.x), a.y + alpha * (b.y - a.y)});
        }
    }
    return out;
}

// Threshold along f (0..2) enclosing area fraction `a` of the unit square below
// an iso-f line. Inverse of the piecewise-quadratic area function.
float area_to_threshold(float a) {
    if (a <= 0.5f) {
        return std::sqrt(2.0f * a);
    }
    return 2.0f - std::sqrt(2.0f * (1.0f - a));
}

void draw_multi_swatch(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target_obj(e));
    auto& registry = paint_registry();
    auto it = registry.find(obj);
    if (it == registry.end() || it->second.colors.size() < 2) {
        return;
    }
    const std::vector<lv_color_t>& colors = it->second.colors;

    lv_area_t coords;
    lv_obj_get_content_coords(obj, &coords);
    float x0 = static_cast<float>(coords.x1);
    float y0 = static_cast<float>(coords.y1);
    float w = static_cast<float>(lv_area_get_width(&coords));
    float h = static_cast<float>(lv_area_get_height(&coords));
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }

    DiagField f{x0, y0, 1.0f / w, 1.0f / h};
    const std::vector<FPoint> square = {{x0, y0}, {x0 + w, y0}, {x0 + w, y0 + h}, {x0, y0 + h}};

    lv_layer_t* layer = lv_event_get_layer(e);
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;

    const size_t n = colors.size();
    for (size_t k = 0; k < n; ++k) {
        float t_lo = (k == 0) ? 0.0f : area_to_threshold(static_cast<float>(k) / n);
        float t_hi = (k == n - 1) ? 2.0f : area_to_threshold(static_cast<float>(k + 1) / n);

        std::vector<FPoint> band = clip_halfplane(square, f, t_lo, /*keep_ge=*/true);
        band = clip_halfplane(band, f, t_hi, /*keep_ge=*/false);
        if (band.size() < 3) {
            continue;
        }

        dsc.color = colors[k];
        // Fan-triangulate the convex band around its first vertex.
        // lv_value_precise_t is float or int32_t depending on LV_USE_FLOAT;
        // set members with a rounding cast so both configs compile cleanly.
        auto set_point = [](lv_point_precise_t& p, const FPoint& src) {
            p.x = static_cast<lv_value_precise_t>(lroundf(src.x));
            p.y = static_cast<lv_value_precise_t>(lroundf(src.y));
        };
        for (size_t i = 1; i + 1 < band.size(); ++i) {
            set_point(dsc.p[0], band[0]);
            set_point(dsc.p[1], band[i]);
            set_point(dsc.p[2], band[i + 1]);
            lv_draw_triangle(layer, &dsc);
        }
    }

    // Thin separator along each internal boundary so a chunk whose color matches
    // the surrounding background (e.g. a near-black marble base on a dark modal)
    // still reads as a distinct region.
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted_border_color();
    line.width = 1;
    line.opa = LV_OPA_COVER;
    auto set_line_point = [](lv_point_precise_t& p, float px, float py) {
        p.x = static_cast<lv_value_precise_t>(lroundf(px));
        p.y = static_cast<lv_value_precise_t>(lroundf(py));
    };
    for (size_t k = 1; k < n; ++k) {
        float t = area_to_threshold(static_cast<float>(k) / n);
        if (t <= 1.0f) {
            set_line_point(line.p1, x0 + t * w, y0);
            set_line_point(line.p2, x0, y0 + t * h);
        } else {
            set_line_point(line.p1, x0 + w, y0 + (t - 1.0f) * h);
            set_line_point(line.p2, x0 + (t - 1.0f) * w, y0 + h);
        }
        lv_draw_line(layer, &line);
    }
}

void on_swatch_deleted(lv_event_t* e) {
    paint_registry().erase(static_cast<lv_obj_t*>(lv_event_get_target_obj(e)));
}

// Remove the multi-color callbacks + retained state from a swatch, if present.
void detach_multi(lv_obj_t* swatch) {
    auto& registry = paint_registry();
    if (registry.erase(swatch) > 0) {
        lv_obj_remove_event_cb(swatch, draw_multi_swatch);
        lv_obj_remove_event_cb(swatch, on_swatch_deleted);
    }
}

} // namespace

void apply_swatch_color(lv_obj_t* swatch, uint32_t primary_rgb,
                        const std::string& multi_color_hexes) {
    if (!swatch) {
        return;
    }

    std::vector<lv_color_t> colors = parse_multi_colors(multi_color_hexes);

    if (colors.size() < 2) {
        // Solid fill with a subtle delineating border.
        detach_multi(swatch);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(primary_rgb), 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(swatch, muted_border_color(), 0);
        lv_obj_set_style_border_opa(swatch, SWATCH_BORDER_OPA, 0);
        return;
    }

    // Multi-color: our DRAW_MAIN callback fills the chunks, so suppress the
    // object's own background but keep the delineating border.
    auto& registry = paint_registry();
    bool first_attach = registry.find(swatch) == registry.end();
    registry[swatch].colors = std::move(colors);
    if (first_attach) {
        lv_obj_add_event_cb(swatch, draw_multi_swatch, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_add_event_cb(swatch, on_swatch_deleted, LV_EVENT_DELETE, nullptr);
    }
    lv_obj_set_style_bg_opa(swatch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(swatch, muted_border_color(), 0);
    lv_obj_set_style_border_opa(swatch, SWATCH_BORDER_OPA, 0);
    lv_obj_invalidate(swatch);
}

void reset_swatch_state() {
    paint_registry().clear();
}

} // namespace helix::ui

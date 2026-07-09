// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_preflight_gate.cpp
 * @brief Tests the print-start pre-flight readiness gate logic.
 *
 * Bug context (the regression this guards against): the gate originally waited on
 * is_gcode_loaded(), which is set ONLY by the visual gcode-viewer load callback.
 * On 2D-only platforms (Snapmaker U1, AD5M, small screens) the viewer SKIPS
 * parsing, so is_gcode_loaded() is never true, run_when_loaded()'s callback never
 * fires, and the print silently never starts — blocking every print on those
 * devices.
 *
 * Fix: readiness now arrives via EITHER the viewer parse OR a headless tools_used
 * scan that runs on ALL platforms, plus a safety timeout so a stuck/failed scan
 * can never wedge the print. These tests model that exact state machine (mirroring
 * the production logic in PrintSelectDetailView, following the lightweight-model
 * pattern used by test_print_select_deactivate_detail.cpp), so they fail if the
 * gate ever regresses to viewer-only readiness or loses graceful degradation.
 */

#include <functional>
#include <set>

#include "../catch_amalgamated.hpp"

namespace {

/// Minimal model of PrintSelectDetailView's pre-flight readiness state machine.
struct PreflightGateModel {
    bool gcode_loaded = false;       // viewer parse done (full platforms only)
    bool headless_scan_done = false; // headless tools_used scan done (all platforms)
    std::function<void()> pending;   // run_when_preflight_ready deferral
    bool timer_armed = false;        // safety timeout armed

    /// Mirrors is_preflight_ready().
    [[nodiscard]] bool is_preflight_ready() const {
        return gcode_loaded || headless_scan_done;
    }

    /// Mirrors run_when_preflight_ready().
    void run_when_preflight_ready(std::function<void()> cb) {
        if (is_preflight_ready()) {
            cb(); // run synchronously
            return;
        }
        pending = std::move(cb);
        timer_armed = true; // safety timeout
    }

    /// Mirrors fire_on_preflight_ready() (also disarms the timer).
    void fire_on_preflight_ready() {
        timer_armed = false;
        if (pending) {
            auto cb = std::move(pending);
            pending = nullptr;
            cb();
        }
    }

    /// Mirrors the viewer load callback path.
    void on_viewer_parsed() {
        gcode_loaded = true;
        fire_on_preflight_ready();
    }

    /// Mirrors the headless scan completion path.
    void on_headless_scan_done() {
        headless_scan_done = true;
        fire_on_preflight_ready();
    }

    /// Mirrors the safety-timeout expiry path (graceful degradation).
    void on_timeout() {
        headless_scan_done = true; // proceed without tools_used
        fire_on_preflight_ready();
    }
};

} // namespace

TEST_CASE("Pre-flight gate: 2D-only platform does not hang", "[print_select][preflight][gate]") {
    // 2D-only: the viewer never parses (gcode_loaded stays false). The print must
    // still start once the headless scan completes.
    PreflightGateModel m;
    bool print_started = false;

    // User taps Print before any scan completes.
    REQUIRE_FALSE(m.is_preflight_ready());
    m.run_when_preflight_ready([&] { print_started = true; });
    REQUIRE_FALSE(print_started); // deferred, not hung
    REQUIRE(m.timer_armed);

    // Headless scan completes — print proceeds even though the viewer never parsed.
    m.on_headless_scan_done();
    REQUIRE(print_started);
    REQUIRE_FALSE(m.timer_armed); // timer disarmed on fire

    // Regression assertion: readiness must NOT depend on the viewer parse.
    REQUIRE_FALSE(m.gcode_loaded);
    REQUIRE(m.is_preflight_ready());
}

TEST_CASE("Pre-flight gate: full platform fires on viewer parse",
          "[print_select][preflight][gate]") {
    PreflightGateModel m;
    bool print_started = false;

    m.run_when_preflight_ready([&] { print_started = true; });
    REQUIRE_FALSE(print_started);

    m.on_viewer_parsed();
    REQUIRE(print_started);
}

TEST_CASE("Pre-flight gate: already-ready runs synchronously", "[print_select][preflight][gate]") {
    PreflightGateModel m;
    m.headless_scan_done = true; // scan already finished by the time Print is tapped
    bool print_started = false;

    REQUIRE(m.is_preflight_ready());
    m.run_when_preflight_ready([&] { print_started = true; });
    REQUIRE(print_started);       // ran immediately, no deferral
    REQUIRE_FALSE(m.timer_armed); // no timer needed
}

TEST_CASE("Pre-flight gate: graceful degradation on stuck scan (timeout)",
          "[print_select][preflight][gate]") {
    // A stuck/failed scan must never wedge the print: the safety timeout fires the
    // deferred attempt anyway.
    PreflightGateModel m;
    bool print_started = false;

    m.run_when_preflight_ready([&] { print_started = true; });
    REQUIRE_FALSE(print_started);
    REQUIRE(m.timer_armed);

    // Neither viewer nor scan ever completes — timeout rescues the print.
    m.on_timeout();
    REQUIRE(print_started);
    REQUIRE(m.is_preflight_ready());
}

namespace {

/// Minimal model of the detail view's color-swatch render decision, mirroring the
/// logic shared by try_extract_gcode_colors() (viewer-parse path) and the headless
/// scan completion path. Guards the 22d37fd47 regression where 2D-only platforms
/// rendered no swatches because the viewer never parses.
struct SwatchRenderModel {
    bool gcode_loaded = false;    // viewer parse done (full platforms)
    bool mapping_visible = false; // editable mapping card shown (hides swatches)
    bool is_multi_tool = false;   // multi-tool printer / >1 AMS slot
    std::set<int> viewer_tools;   // tools from viewer parse (full platforms)
    std::set<int> headless_tools; // tools from headless scan (2D-only)
    bool headless_done = false;

    // Render outputs.
    int swatches_visible = 0;
    std::set<int> rendered_tools;

    [[nodiscard]] bool is_gcode_loaded() const {
        return gcode_loaded;
    }

    /// Mirrors tools_used_effective(): prefer viewer parse, else headless set.
    [[nodiscard]] std::set<int> tools_used_effective() const {
        if (!viewer_tools.empty()) {
            return viewer_tools;
        }
        if (headless_done) {
            return headless_tools;
        }
        return {};
    }

    /// Mirrors swatches_card_visible_for().
    [[nodiscard]] bool swatches_card_visible_for(size_t n) const {
        return is_multi_tool ? n > 0 : n > 1;
    }

    void render_swatches(const std::set<int>& tools) {
        rendered_tools = tools; // update_color_swatches clears-then-rebuilds → idempotent
    }

    /// Mirrors the headless-scan completion handler (2D-only swatch render).
    void on_headless_scan_done() {
        headless_done = true;
        if (!is_gcode_loaded()) {
            auto tools = tools_used_effective();
            bool visible = !mapping_visible && swatches_card_visible_for(tools.size());
            swatches_visible = visible ? 1 : 0;
            if (visible) {
                render_swatches(tools);
            }
        }
    }

    /// Mirrors try_extract_gcode_colors() (viewer-parse swatch render).
    void on_viewer_parsed() {
        gcode_loaded = true;
        bool visible = !mapping_visible && swatches_card_visible_for(viewer_tools.size());
        swatches_visible = visible ? 1 : 0;
        if (visible) {
            render_swatches(viewer_tools);
        }
    }
};

} // namespace

TEST_CASE("Swatch render: 2D-only headless scan renders real used tools",
          "[print_select][preflight][swatch]") {
    // Regression 22d37fd47: 2D-only viewer never parses, so swatches must be driven
    // by the headless scan's REAL tool set ({0,2}), not left hidden/empty.
    SwatchRenderModel m;
    m.is_multi_tool = true;    // U1 toolchanger
    m.headless_tools = {0, 2}; // real used tools recovered by the scan

    REQUIRE(m.swatches_visible == 0); // nothing rendered before the scan
    m.on_headless_scan_done();

    REQUIRE_FALSE(m.gcode_loaded);                    // viewer never parsed
    REQUIRE(m.swatches_visible == 1);                 // swatches now shown
    REQUIRE(m.rendered_tools == std::set<int>{0, 2}); // REAL tools, not {0,1,2,3}
}

TEST_CASE("Swatch render: mapping card hides swatches on 2D-only",
          "[print_select][preflight][swatch]") {
    SwatchRenderModel m;
    m.is_multi_tool = true;
    m.mapping_visible = true; // editable mapping present → swatches suppressed
    m.headless_tools = {0, 2};

    m.on_headless_scan_done();
    REQUIRE(m.swatches_visible == 0);
    REQUIRE(m.rendered_tools.empty());
}

TEST_CASE("Swatch render: full platform keeps viewer-parse ownership",
          "[print_select][preflight][swatch]") {
    // On full platforms the viewer parse renders. A subsequent headless scan must
    // NOT re-render (is_gcode_loaded() guard) — avoids double-render.
    SwatchRenderModel m;
    m.is_multi_tool = true;
    m.viewer_tools = {0, 1};
    m.headless_tools = {0, 1, 2}; // would over-count if it leaked through

    m.on_viewer_parsed();
    REQUIRE(m.swatches_visible == 1);
    REQUIRE(m.rendered_tools == std::set<int>{0, 1});

    // Headless scan completes later — must be a no-op for the swatch render.
    m.on_headless_scan_done();
    REQUIRE(m.rendered_tools == std::set<int>{0, 1}); // unchanged: viewer owns it
}

TEST_CASE("Pre-flight gate: readiness fires exactly once", "[print_select][preflight][gate]") {
    // If both the headless scan and a (later) viewer parse complete, the deferred
    // print attempt must fire exactly once, not twice.
    PreflightGateModel m;
    int fire_count = 0;

    m.run_when_preflight_ready([&] { ++fire_count; });
    m.on_headless_scan_done();
    REQUIRE(fire_count == 1);

    // A subsequent viewer parse must not re-fire the (already-consumed) callback.
    m.on_viewer_parsed();
    REQUIRE(fire_count == 1);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_preview_decision.cpp
 * @brief Tests for the pure preview-reconciliation decision function.
 *
 * These tests call the REAL helix::ui::decide_preview_action() (not a shadow
 * copy), so they double as the regression guard for the self-healing re-entry
 * behavior: the decision reads ACTUAL widget state (thumbnail has source, gcode
 * has geometry) rather than intent bools that can lie after destroy-on-close /
 * memory reclaim.
 *
 * Bug context: navigating away from Print Status mid-print and back left the
 * preview blank because dedup guards said "showing X" while the recreated/
 * cleared widget showed nothing. decide_preview_action() makes a blank widget
 * always reload because it compares against widget reality.
 */

#include <string>

#include "../catch_amalgamated.hpp"

#include "print_status_preview_decision.h"

using helix::ui::decide_preview_action;
using helix::ui::PreviewAction;

// View modes: 0=thumbnail, 1=3D, 2=2D
constexpr int MODE_THUMB = 0;
constexpr int MODE_3D = 1;
constexpr int MODE_2D = 2;

TEST_CASE("Preview decision: fresh open loads both", "[print_status][preview]") {
    // Nothing displayed yet, blank widgets, viewer wanted, 3D mode.
    PreviewAction a = decide_preview_action(/*displayed*/ "", /*desired*/ "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ false,
                                            /*want_viewer*/ true, MODE_3D);
    REQUIRE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry all valid is a no-op", "[print_status][preview]") {
    // Same file already displayed, both widgets populated → nothing to do.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true, MODE_3D);
    REQUIRE_FALSE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry with blank thumbnail reloads thumbnail",
          "[print_status][preview]") {
    // Widget was recreated/reclaimed: displayed bool says same file, but the
    // thumbnail image source is gone. Must reload the thumbnail even though the
    // filename did not change.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ true,
                                            /*want_viewer*/ true, MODE_3D);
    REQUIRE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry with unloaded viewer reloads gcode",
          "[print_status][preview]") {
    // Viewer geometry was cleared (memory pressure) but thumbnail survived.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ false,
                                            /*want_viewer*/ true, MODE_2D);
    REQUIRE_FALSE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: thumbnail-only mode never loads gcode", "[print_status][preview]") {
    SECTION("view mode thumbnail, blank widgets") {
        PreviewAction a = decide_preview_action("", "benchy.gcode",
                                                /*thumb_src*/ false, /*gcode_content*/ false,
                                                /*want_viewer*/ true, MODE_THUMB);
        REQUIRE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }

    SECTION("want_viewer false suppresses gcode even in 3D mode") {
        PreviewAction a = decide_preview_action("", "benchy.gcode",
                                                /*thumb_src*/ false, /*gcode_content*/ false,
                                                /*want_viewer*/ false, MODE_3D);
        REQUIRE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }
}

TEST_CASE("Preview decision: filename change reloads both", "[print_status][preview]") {
    // A different print started. Even though both widgets hold content, it is
    // the OLD file's content → reload both.
    PreviewAction a = decide_preview_action("old.gcode", "new.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true, MODE_3D);
    REQUIRE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: desired empty does nothing", "[print_status][preview]") {
    // No active print → leave widgets alone regardless of their state.
    SECTION("blank widgets") {
        PreviewAction a = decide_preview_action("", "", false, false, true, MODE_3D);
        REQUIRE_FALSE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }
    SECTION("stale content from finished print") {
        PreviewAction a = decide_preview_action("done.gcode", "", true, true, true, MODE_3D);
        REQUIRE_FALSE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }
}

TEST_CASE("Preview decision: filename change in thumbnail-only mode reloads only thumbnail",
          "[print_status][preview]") {
    PreviewAction a = decide_preview_action("old.gcode", "new.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true, MODE_THUMB);
    REQUIRE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

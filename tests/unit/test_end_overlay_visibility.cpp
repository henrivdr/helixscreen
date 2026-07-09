// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_end_overlay_visibility.cpp
 * @brief Pure-logic tests for the end-of-print overlay visibility rule
 *
 * Mirrors PrintStatusPanel::recompute_end_overlay_visibility(): the three
 * derived show_{complete,cancelled,error}_overlay flags are each true iff
 * print_outcome matches the overlay AND end_overlay_dismissed is 0. Replaces
 * the previous racy pair of XML bind_flag observers (L042) that unhid the
 * error overlay at startup when end_overlay_dismissed defaulted to 0.
 */

#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using helix::PrintOutcome;

namespace {

struct OverlayFlags {
    int complete;
    int cancelled;
    int error;
};

// Pure mirror of PrintStatusPanel::recompute_end_overlay_visibility. Kept
// outside the class so it can be tested without spinning up LVGL.
static OverlayFlags compute_flags(PrintOutcome outcome, bool dismissed) {
    return {
        (!dismissed && outcome == PrintOutcome::COMPLETE) ? 1 : 0,
        (!dismissed && outcome == PrintOutcome::CANCELLED) ? 1 : 0,
        (!dismissed && outcome == PrintOutcome::ERROR) ? 1 : 0,
    };
}

} // namespace

TEST_CASE("End overlay visibility — at most one overlay visible at a time",
          "[print_status][overlay]") {
    SECTION("NONE outcome hides all three overlays (startup state)") {
        auto f = compute_flags(PrintOutcome::NONE, /*dismissed=*/false);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }

    SECTION("COMPLETE outcome shows only complete") {
        auto f = compute_flags(PrintOutcome::COMPLETE, false);
        REQUIRE(f.complete == 1);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }

    SECTION("CANCELLED outcome shows only cancelled") {
        auto f = compute_flags(PrintOutcome::CANCELLED, false);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 1);
        REQUIRE(f.error == 0);
    }

    SECTION("ERROR outcome shows only error") {
        auto f = compute_flags(PrintOutcome::ERROR, false);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 1);
    }
}

TEST_CASE("End overlay dismissal suppresses all three overlays", "[print_status][overlay]") {
    SECTION("Dismissed COMPLETE stays hidden") {
        auto f = compute_flags(PrintOutcome::COMPLETE, /*dismissed=*/true);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }

    SECTION("Dismissed CANCELLED stays hidden") {
        auto f = compute_flags(PrintOutcome::CANCELLED, true);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }

    SECTION("Dismissed ERROR stays hidden — the startup-race guard") {
        // Regression guard for L042: the old XML let end_overlay_dismissed=0
        // unhide the error overlay before any print had failed.
        auto f = compute_flags(PrintOutcome::ERROR, true);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }

    SECTION("Dismissed NONE stays hidden") {
        auto f = compute_flags(PrintOutcome::NONE, true);
        REQUIRE(f.complete == 0);
        REQUIRE(f.cancelled == 0);
        REQUIRE(f.error == 0);
    }
}

TEST_CASE("End overlay truth table — full outcome × dismissed matrix", "[print_status][overlay]") {
    struct Case {
        PrintOutcome outcome;
        bool dismissed;
        int expect_complete;
        int expect_cancelled;
        int expect_error;
    };

    const Case cases[] = {
        {PrintOutcome::NONE, false, 0, 0, 0},      {PrintOutcome::NONE, true, 0, 0, 0},
        {PrintOutcome::COMPLETE, false, 1, 0, 0},  {PrintOutcome::COMPLETE, true, 0, 0, 0},
        {PrintOutcome::CANCELLED, false, 0, 1, 0}, {PrintOutcome::CANCELLED, true, 0, 0, 0},
        {PrintOutcome::ERROR, false, 0, 0, 1},     {PrintOutcome::ERROR, true, 0, 0, 0},
    };

    for (const auto& c : cases) {
        auto f = compute_flags(c.outcome, c.dismissed);
        CAPTURE(static_cast<int>(c.outcome), c.dismissed);
        REQUIRE(f.complete == c.expect_complete);
        REQUIRE(f.cancelled == c.expect_cancelled);
        REQUIRE(f.error == c.expect_error);
    }
}

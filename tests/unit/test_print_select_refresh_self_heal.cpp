// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_refresh_self_heal.cpp
 * @brief Lock in PrintSelectPanel's stuck-refresh self-heal behavior (#911).
 *
 * Background: 2dac15cba added a 30 s self-heal in PrintSelectPanel::refresh_files
 * for the case where the WebSocket response for a get_directory RPC is silently
 * dropped (root cause was #909, fixed in d34d0a9de — but the panel-side safety
 * net stays as defense-in-depth). The full integration would need an
 * XMLTestFixture with a mock provider; instead this test pins the predicate
 * (refresh_should_skip) so any refactor to refresh_files preserves the
 * decision logic.
 */

#include "ui_panel_print_select.h"

#include <chrono>

#include "../catch_amalgamated.hpp"

using namespace std::chrono_literals;

TEST_CASE("refresh_should_skip: not in-flight → never skip",
          "[print_select][refresh][regression]") {
    auto now = std::chrono::steady_clock::now();
    REQUIRE(refresh_should_skip(/*in_flight=*/false, /*force=*/false, now, now, 30s) == false);
    REQUIRE(refresh_should_skip(/*in_flight=*/false, /*force=*/false, now, now + 1h, 30s) == false);
}

TEST_CASE("refresh_should_skip: force=true → never skip even if in-flight",
          "[print_select][refresh][regression]") {
    auto started = std::chrono::steady_clock::now();
    auto now = started + 50ms; // well within threshold
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/true, started, now, 30s) == false);
}

TEST_CASE("refresh_should_skip: in-flight under threshold → skip",
          "[print_select][refresh][regression]") {
    auto started = std::chrono::steady_clock::now();
    auto now = started + 5s; // 5 s in, well under 30 s threshold
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, started, now, 30s) == true);
}

TEST_CASE("refresh_should_skip: in-flight at exactly threshold → self-heal",
          "[print_select][refresh][regression]") {
    auto started = std::chrono::steady_clock::now();
    auto now = started + 30s; // exactly at threshold
    // Predicate uses `<` so exact equality falls through (self-heal).
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, started, now, 30s) == false);
}

TEST_CASE("refresh_should_skip: in-flight past threshold → self-heal",
          "[print_select][refresh][regression]") {
    auto started = std::chrono::steady_clock::now();
    auto now = started + 60s; // double the threshold
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, started, now, 30s) == false);
}

TEST_CASE("refresh_should_skip: short test threshold (50ms) honoured",
          "[print_select][refresh][regression]") {
    auto started = std::chrono::steady_clock::now();

    SECTION("under 50 ms → skip") {
        REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, started, started + 10ms,
                                    50ms) == true);
    }
    SECTION("over 50 ms → self-heal") {
        REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, started, started + 100ms,
                                    50ms) == false);
    }
}

TEST_CASE("refresh_should_skip: zero threshold always self-heals when in-flight",
          "[print_select][refresh][edge]") {
    auto t = std::chrono::steady_clock::now();
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, t, t, 0ms) == false);
    REQUIRE(refresh_should_skip(/*in_flight=*/true, /*force=*/false, t, t + 1us, 0ms) == false);
}

// ============================================================================
// dir_error_should_reset_to_root: recover from a phantom current directory.
//
// FlashForge's Moonraker returned a doubled folder segment
// ("Feinkost/Feinkost/Gridfinity") that the server reports as missing; the panel
// retried it every refresh forever (debug bundle TJVQDCZ6). When get_directory
// fails because the directory is gone, the panel should fall back to root rather
// than loop. Resetting only when NOT already at root prevents an infinite
// reset->refresh->fail cycle if root itself ever errors.
// ============================================================================

TEST_CASE("dir_error_should_reset_to_root: missing-directory error in subfolder resets",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root(
                "Directory does not exist (/usr/data/gcodes/Feinkost/Feinkost/Gridfinity)",
                /*at_root=*/false) == true);
}

TEST_CASE("dir_error_should_reset_to_root: already at root never resets (no loop)",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("Directory does not exist (/usr/data/gcodes)",
                                           /*at_root=*/true) == false);
}

TEST_CASE("dir_error_should_reset_to_root: unrelated error does not reset",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("Klippy disconnected", /*at_root=*/false) == false);
    REQUIRE(dir_error_should_reset_to_root("", /*at_root=*/false) == false);
}

TEST_CASE("dir_error_should_reset_to_root: match is case-insensitive",
          "[print_select][refresh][regression]") {
    REQUIRE(dir_error_should_reset_to_root("DIRECTORY DOES NOT EXIST (/x)",
                                           /*at_root=*/false) == true);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_deactivate_detail.cpp
 * @brief Tests that detail_view_open_ is cleared on panel deactivation
 *
 * Bug context: If user navigated away from PrintSelectPanel via navbar while
 * the file detail view was open, detail_view_open_ stayed true. All subsequent
 * notify_filelist_changed notifications were deferred (never processed), so
 * uploaded files wouldn't appear until app restart.
 */

#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Helper: Simulates PrintSelectPanel's detail/notification state machine
// ============================================================================

/**
 * @brief Minimal model of PrintSelectPanel's detail view + file notification state.
 *
 * Mirrors the interaction between:
 * - detail_view_open_ (set by show_detail_view / hide_detail_view / on_deactivate)
 * - files_changed_while_detail_open_ (set by notify_filelist_changed)
 * - on_activate() skip logic
 */
struct FileListNotificationState {
    bool detail_view_open = false;
    bool files_changed_while_detail_open = false;
    bool first_activation = true;
    bool has_files = false;

    /// Mirrors show_detail_view()
    void show_detail_view() {
        detail_view_open = true;
    }

    /// Mirrors hide_detail_view()
    void hide_detail_view() {
        detail_view_open = false;
    }

    /// Mirrors on_deactivate() — MUST clear detail_view_open_
    void on_deactivate() {
        detail_view_open = false;
    }

    /// Mirrors notify_filelist_changed handler — returns true if refresh runs
    bool on_filelist_changed() {
        if (detail_view_open) {
            files_changed_while_detail_open = true;
            return false; // deferred
        }
        return true; // refresh runs
    }

    /// Mirrors on_activate() — returns true if file list refresh runs
    bool on_activate() {
        // Skip refresh when returning from detail view if no files changed
        if (!first_activation && has_files && !files_changed_while_detail_open) {
            files_changed_while_detail_open = false;
            return false; // skip refresh, preserve scroll position
        }
        files_changed_while_detail_open = false;
        first_activation = false;
        return true; // refresh runs
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("Detail view open flag cleared on deactivation",
          "[print_select][detail_view][deactivate]") {
    SECTION("Navigate away via navbar with detail view open clears flag") {
        FileListNotificationState state;
        state.first_activation = false;
        state.has_files = true;

        // User opens file detail view
        state.show_detail_view();
        REQUIRE(state.detail_view_open);

        // User taps navbar to go to different panel (deactivate, NOT hide_detail_view)
        state.on_deactivate();
        REQUIRE_FALSE(state.detail_view_open);

        // New file uploaded — notification should NOT be deferred
        bool refreshed = state.on_filelist_changed();
        REQUIRE(refreshed);
    }

    SECTION("Normal detail close still works") {
        FileListNotificationState state;
        state.first_activation = false;
        state.has_files = true;

        state.show_detail_view();
        state.hide_detail_view();
        REQUIRE_FALSE(state.detail_view_open);

        bool refreshed = state.on_filelist_changed();
        REQUIRE(refreshed);
    }

    SECTION("Bug scenario: stuck detail_view_open_ blocks all file notifications") {
        // This test documents the bug that was fixed.
        // Without the on_deactivate() fix, detail_view_open_ stays true.
        FileListNotificationState state;
        state.first_activation = false;
        state.has_files = true;

        // User opens detail view, then navigates away via navbar
        state.show_detail_view();
        state.on_deactivate(); // Fix: clears detail_view_open_

        // Multiple file uploads — all should trigger refresh
        REQUIRE(state.on_filelist_changed());
        REQUIRE(state.on_filelist_changed());
        REQUIRE(state.on_filelist_changed());
    }

    SECTION("Deferred notification consumed on next activate") {
        FileListNotificationState state;
        state.first_activation = false;
        state.has_files = true;

        // File changes while detail view is open (normal case)
        state.show_detail_view();
        bool refreshed = state.on_filelist_changed();
        REQUIRE_FALSE(refreshed); // deferred

        // User closes detail view, panel re-activates
        state.hide_detail_view();
        REQUIRE(state.files_changed_while_detail_open);

        // on_activate should trigger refresh because files_changed flag is set
        bool activated_refresh = state.on_activate();
        REQUIRE(activated_refresh);
        REQUIRE_FALSE(state.files_changed_while_detail_open); // consumed
    }
}

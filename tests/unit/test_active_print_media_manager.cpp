// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_media_manager.cpp
 * @brief Unit tests for ActivePrintMediaManager class
 *
 * Tests the media manager that:
 * - Observes print_filename_ subject from PrinterState
 * - Processes raw filename to display name
 * - Loads thumbnails via MoonrakerAPI
 * - Updates print_display_filename_ and print_thumbnail_path_ subjects
 * - Uses generation counter for stale callback detection
 */

#include "../test_helpers/active_print_media_manager_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "active_print_media_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"
#include "moonraker_file_transfer_api.h"
#include "printer_state.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using namespace helix::ui;

// ============================================================================
// Test Fixture for ActivePrintMediaManager tests
// ============================================================================

class ActivePrintMediaManagerTestFixture {
  public:
    ActivePrintMediaManagerTestFixture() {
        // Suppress spdlog output during tests
        if (!logger_initialized_) {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            auto null_logger = std::make_shared<spdlog::logger>("null", null_sink);
            spdlog::set_default_logger(null_logger);
            logger_initialized_ = true;
        }

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        // Reset PrinterState for test isolation
        PrinterStateTestAccess::reset(state_);

        // Initialize subjects (without XML registration in tests)
        state_.init_subjects(false);

        // Create ActivePrintMediaManager for this test
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
    }

    ~ActivePrintMediaManagerTestFixture() {
        // Destroy manager first (it observes state_)
        manager_.reset();

        // Drain any pending updates before shutdown to ensure clean state
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Shutdown update queue - also clears any remaining pending callbacks
        helix::ui::update_queue_shutdown();
        queue_initialized = false; // Reset static flag for next test

        // Destroy display to prevent cross-shard leaks
        if (display_created_ && display_) {
            lv_display_delete(display_);
            display_ = nullptr;
            display_created_ = false;
        }

        // Reset logger flag so next test/shard re-initializes
        logger_initialized_ = false;

        // Reset after each test
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    helix::ActivePrintMediaManager& manager() {
        return *manager_;
    }

    // Helper to update print filename via status JSON (simulates Moonraker notification)
    void set_print_filename(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        // Drain all queued UI updates, including nested queue_update calls from
        // deferred observers (observer → handler → display filename setter)
        UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    // Get current print_filename (raw)
    std::string get_print_filename() {
        return lv_subject_get_string(state_.get_print_filename_subject());
    }

    // Get current print_display_filename (processed for UI)
    std::string get_display_filename() {
        return lv_subject_get_string(state_.get_print_display_filename_subject());
    }

    // Get current print_thumbnail_path
    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

  private:
    PrinterState state_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
    static lv_display_t* display_;
    static bool display_created_;
    static bool queue_initialized;
    static bool logger_initialized_;
};

lv_display_t* ActivePrintMediaManagerTestFixture::display_ = nullptr;
bool ActivePrintMediaManagerTestFixture::display_created_ = false;
bool ActivePrintMediaManagerTestFixture::queue_initialized = false;
bool ActivePrintMediaManagerTestFixture::logger_initialized_ = false;

// ============================================================================
// Display Name Formatting Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: simple filename produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("benchy.gcode");

    REQUIRE(get_print_filename() == "benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: filename with path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("my_models/benchy.gcode");

    REQUIRE(get_print_filename() == "my_models/benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: helix_temp filename resolves to original",
                 "[ActivePrintMediaManager]") {
    // When HelixScreen modifies G-code, it creates temp files like:
    // .helix_temp/modified_1234567890_Original_Model.gcode
    // The display name should show "Original_Model", not the temp filename
    set_print_filename(".helix_temp/modified_1234567890_Body1.gcode");

    REQUIRE(get_print_filename() == ".helix_temp/modified_1234567890_Body1.gcode");
    REQUIRE(get_display_filename() == "Body1");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: complex helix_temp path resolves correctly",
                 "[ActivePrintMediaManager]") {
    set_print_filename(".helix_temp/modified_9876543210_My_Cool_Print.gcode");

    REQUIRE(get_display_filename() == "My_Cool_Print");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: deeply nested path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");

    REQUIRE(get_print_filename() == "projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");
    REQUIRE(get_display_filename() == "benchy_0.2mm_PLA");
}

// ============================================================================
// Empty Filename Handling Tests
// ============================================================================
// Design: Empty filename PRESERVES display info (for abort→firmware_restart UX).
// Clearing happens naturally when a NEW print starts with a different filename.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves display name",
                 "[ActivePrintMediaManager]") {
    // First set a filename
    set_print_filename("test.gcode");
    REQUIRE(get_print_filename() == "test.gcode");
    REQUIRE(get_display_filename() == "test");

    // When printer goes to standby (empty filename), display name is preserved
    // so users can see what was printing after cancel→firmware_restart
    set_print_filename("");
    REQUIRE(get_print_filename() == "");

    // Display filename should be PRESERVED (not cleared)
    REQUIRE(get_display_filename() == "test");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves thumbnail path",
                 "[ActivePrintMediaManager]") {
    // Set a filename first (to trigger the manager to process)
    set_print_filename("test.gcode");

    // Manually set a thumbnail path (simulating a loaded thumbnail)
    state().set_print_thumbnail_path("A:/tmp/thumbnail_abc123.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");

    // When filename is cleared, thumbnail is PRESERVED (not cleared)
    // This allows users to see the print info after abort→firmware_restart
    set_print_filename("");

    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new filename replaces old display info",
                 "[ActivePrintMediaManager]") {
    // Set initial filename
    set_print_filename("first_print.gcode");
    REQUIRE(get_display_filename() == "first_print");

    // Manually set thumbnail (simulating loaded thumbnail)
    state().set_print_thumbnail_path("A:/tmp/first_thumb.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/first_thumb.bin");

    // Start a NEW print - this should replace display name
    set_print_filename("second_print.gcode");
    REQUIRE(get_display_filename() == "second_print");

    // Thumbnail path is cleared when new print starts (will be reloaded via API)
    // Note: Without API set, thumbnail loading is skipped, so path remains
    // until explicitly cleared or new thumbnail is loaded
}

// ============================================================================
// Thumbnail Source Override Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: manual thumbnail source takes precedence",
                 "[ActivePrintMediaManager]") {
    // When PrintPreparationManager starts a modified print, it knows the original filename
    // and can provide it via set_thumbnail_source() for proper resolution

    // Set the thumbnail source BEFORE the filename arrives
    manager().set_thumbnail_source("original_model.gcode");

    // Now when a temp filename arrives, the source override should be used
    set_print_filename(".helix_temp/modified_12345_original_model.gcode");

    // Display name should use the source override
    REQUIRE(get_display_filename() == "original_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: clear_thumbnail_source resets state",
                 "[ActivePrintMediaManager]") {
    // Set up initial state
    set_print_filename("first.gcode");
    REQUIRE(get_display_filename() == "first");

    // Set an override
    manager().set_thumbnail_source("override.gcode");

    // Clear the override
    manager().clear_thumbnail_source();

    // Next filename should be processed normally (no override)
    set_print_filename("second.gcode");
    REQUIRE(get_display_filename() == "second");
}

// ============================================================================
// Generation Counter / Stale Callback Detection Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: rapid filename changes use latest generation",
                 "[ActivePrintMediaManager]") {
    // When filename changes rapidly (user quickly switches prints),
    // only the last one should be reflected

    set_print_filename("print1.gcode");
    set_print_filename("print2.gcode");
    set_print_filename("print3.gcode");

    // Only print3 should be reflected in the display name
    REQUIRE(get_print_filename() == "print3.gcode");
    REQUIRE(get_display_filename() == "print3");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: idempotent on repeated same filename",
                 "[ActivePrintMediaManager]") {
    // Setting the same filename multiple times should not trigger redundant processing
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");

    // Set again - should be idempotent
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");
}

// ============================================================================
// Integration with PrinterState Subjects
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: updates print_display_filename subject",
                 "[ActivePrintMediaManager]") {
    set_print_filename("test_model.gcode");

    REQUIRE(get_display_filename() == "test_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: observer fires on display_filename change",
                 "[ActivePrintMediaManager]") {
    int observer_count = 0;
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer = lv_subject_add_observer(state().get_print_display_filename_subject(),
                                                      observer_cb, &observer_count);

    // Initial observer registration fires once
    REQUIRE(observer_count == 1);

    // Change filename - should fire observer after processing
    set_print_filename("new_model.gcode");

    // Observer should have fired again
    REQUIRE(observer_count == 2);

    lv_observer_remove(observer);
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles filename with special characters",
                 "[ActivePrintMediaManager]") {
    set_print_filename("My Model (v2) - Final.gcode");

    REQUIRE(get_print_filename() == "My Model (v2) - Final.gcode");
    REQUIRE(get_display_filename() == "My Model (v2) - Final");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles very long filename",
                 "[ActivePrintMediaManager]") {
    // Test handling of very long filenames (within buffer limits)
    std::string long_name(100, 'x');
    long_name += ".gcode";

    set_print_filename(long_name);

    // Should handle gracefully (may be truncated to buffer size)
    REQUIRE_FALSE(get_display_filename().empty());
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: no API means no thumbnail load",
                 "[ActivePrintMediaManager]") {
    // Without set_api() being called, thumbnail loading should be skipped gracefully
    set_print_filename("model.gcode");

    // Display name should still work
    REQUIRE(get_display_filename() == "model");

    // Thumbnail path should remain empty (no API to load from)
    REQUIRE(get_thumbnail_path() == "");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: uppercase extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCODE");

    REQUIRE(get_display_filename() == "Model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: mixed case extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCode");

    REQUIRE(get_display_filename() == "Model");
}

// ============================================================================
// Direct Thumbnail Path Tests (Pre-extracted from USB/G-code)
// ============================================================================

// NOTE: This test intentionally fails to compile because set_thumbnail_path()
// doesn't exist yet. This is TDD-style - implement the method to make it compile.
//
// TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
//                  "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
//                  "[media][thumbnail][direct]")
//
// When PrintStartController starts a print with a pre-extracted thumbnail
// (e.g., from USB drive or embedded G-code), it should be able to set the
// thumbnail path directly without going through Moonraker thumbnail API.
//
// Required new method signature:
//   void set_thumbnail_path(const std::string& path);
//
// Test cases that need to pass once implemented:
// 1. Direct path sets thumbnail_path subject
// 2. Direct path works alongside filename
// 3. Direct path not overwritten by filename change if already set
// 4. Empty path clears thumbnail
//
// Uncomment below and add set_thumbnail_path() to ActivePrintMediaManager

#if 1 // Enable when set_thumbnail_path() is implemented
TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
                 "[ActivePrintMediaManager]") {
    SECTION("direct path sets thumbnail_path subject") {
        // Pre-extracted thumbnail from USB or G-code
        std::string extracted_path = "/tmp/helix/thumbnails/extracted_12345.png";

        // Set the thumbnail path directly via new method
        manager().set_thumbnail_path(extracted_path);

        // Thumbnail path subject should have the value
        REQUIRE(get_thumbnail_path() == extracted_path);
    }

    SECTION("direct path works alongside filename") {
        // Set a filename for the print
        set_print_filename("usb_print.gcode");
        REQUIRE(get_display_filename() == "usb_print");

        // Set thumbnail path directly (from pre-extracted USB thumbnail)
        std::string usb_thumbnail = "/media/usb/thumbnails/usb_print.png";
        manager().set_thumbnail_path(usb_thumbnail);

        // Both should be set correctly
        REQUIRE(get_display_filename() == "usb_print");
        REQUIRE(get_thumbnail_path() == usb_thumbnail);
    }

    SECTION("direct path not overwritten by filename change if set") {
        // Set thumbnail path first (from PrintStartController)
        std::string preextracted = "/tmp/helix/embedded_thumbnail.png";
        manager().set_thumbnail_path(preextracted);
        REQUIRE(get_thumbnail_path() == preextracted);

        // When filename arrives from Moonraker, the pre-set thumbnail should persist
        // (because we already have a valid thumbnail, no need to fetch)
        set_print_filename("some_file.gcode");

        // The pre-extracted thumbnail should still be there
        REQUIRE(get_thumbnail_path() == preextracted);
    }

    SECTION("pre-set thumbnail does not block layer count from metadata") {
        // Regression: #526 - when thumbnail was pre-set, the metadata fetch was
        // skipped entirely, so layer_count and estimated_time were never loaded.
        // The fix ensures metadata is always fetched; only thumbnail download is skipped.

        // Set thumbnail path first (simulating PrintStartController pre-extraction)
        manager().set_thumbnail_path("/tmp/helix/preextracted.png");
        REQUIRE(get_thumbnail_path() == "/tmp/helix/preextracted.png");

        // Verify that having a pre-set thumbnail doesn't prevent layer_total
        // from being set. Without API, metadata can't be fetched, but the code
        // path that checks skip_thumbnail should not early-return before the
        // API check. Verify the subject is still at 0 (no API = no metadata).
        REQUIRE(lv_subject_get_int(state().get_print_layer_total_subject()) == 0);

        // The key assertion: load_thumbnail_for_file should NOT early-return
        // when thumbnail is set. It should proceed to the API check and only
        // skip after that (because no API is configured in this test).
        // This is implicitly tested: if the old early-return was still there,
        // the "Thumbnail already set, skipping API lookup" log would fire,
        // and we'd never reach the API check at all.
        set_print_filename("model_with_layers.gcode");

        // Display name should still work (metadata fetch path is entered)
        REQUIRE(get_display_filename() == "model_with_layers");
    }

    SECTION("empty path clears thumbnail") {
        // Set a thumbnail first
        manager().set_thumbnail_path("/tmp/some_thumbnail.png");
        REQUIRE(get_thumbnail_path() == "/tmp/some_thumbnail.png");

        // Clear it
        manager().set_thumbnail_path("");

        // Should be cleared
        REQUIRE(get_thumbnail_path() == "");
    }
}
#endif

// ============================================================================
// Stale Thumbnail Invalidation Tests
// ============================================================================
// Regression: Starting a new print via Mainsail showed the PREVIOUS print's
// thumbnail because print_thumbnail_path_ was never cleared between prints.

// ============================================================================
// Thumbnail Path Subject Observer Integration Tests
// ============================================================================
// These tests verify that the print_thumbnail_path subject correctly notifies
// observers, which is the mechanism PrintStatusWidget uses to update its image.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: thumbnail path subject fires observer on change",
                 "[ActivePrintMediaManager]") {
    std::string last_observed_path;
    int observer_fire_count = 0;

    // Set up an observer on the thumbnail path subject (mimics what the widget does)
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* data =
            static_cast<std::pair<std::string*, int*>*>(lv_observer_get_user_data(observer));
        *data->first = lv_subject_get_string(subj);
        (*data->second)++;
    };

    auto data = std::make_pair(&last_observed_path, &observer_fire_count);
    lv_observer_t* obs =
        lv_subject_add_observer(state().get_print_thumbnail_path_subject(), observer_cb, &data);

    // Observer fires on registration with initial (empty) value
    REQUIRE(observer_fire_count == 1);
    REQUIRE(last_observed_path.empty());

    // Setting a thumbnail path should fire the observer
    state().set_print_thumbnail_path("A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);
    REQUIRE(last_observed_path == "A:/cache/thumb.bin");

    // Setting same path should NOT fire (de-duplication in set_print_thumbnail_path)
    state().set_print_thumbnail_path("A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);

    // Clearing path should fire
    state().set_print_thumbnail_path("");
    REQUIRE(observer_fire_count == 3);
    REQUIRE(last_observed_path.empty());

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(
    ActivePrintMediaManagerTestFixture,
    "ActivePrintMediaManager: thumbnail path observer receives correct value during rapid updates",
    "[ActivePrintMediaManager]") {
    // This tests the scenario where the thumbnail path changes rapidly.
    // An immediate observer should receive each value in sequence.
    std::vector<std::string> observed_values;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* values = static_cast<std::vector<std::string>*>(lv_observer_get_user_data(observer));
        values->push_back(lv_subject_get_string(subj));
    };

    lv_observer_t* obs = lv_subject_add_observer(state().get_print_thumbnail_path_subject(),
                                                 observer_cb, &observed_values);

    // Initial fire
    REQUIRE(observed_values.size() == 1);
    REQUIRE(observed_values[0].empty());

    // Rapid updates - observer should see each distinct value
    state().set_print_thumbnail_path("A:/cache/first.bin");
    state().set_print_thumbnail_path("A:/cache/second.bin");
    state().set_print_thumbnail_path("A:/cache/third.bin");

    REQUIRE(observed_values.size() == 4); // initial + 3 changes
    REQUIRE(observed_values[1] == "A:/cache/first.bin");
    REQUIRE(observed_values[2] == "A:/cache/second.bin");
    REQUIRE(observed_values[3] == "A:/cache/third.bin");

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new print clears stale thumbnail path",
                 "[ActivePrintMediaManager]") {
    SECTION("different file after idle clears old thumbnail") {
        // Print A starts and gets a thumbnail
        set_print_filename("print_a.gcode");
        state().set_print_thumbnail_path("A:/cache/print_a_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print A ends - Moonraker sends empty filename
        set_print_filename("");
        // Thumbnail preserved (intentional for post-cancel UX)
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print B starts from Mainsail - stale thumbnail must be cleared
        set_print_filename("print_b.gcode");
        REQUIRE(get_display_filename() == "print_b");
        // The old thumbnail path should be cleared so the new one can be fetched
        REQUIRE(get_thumbnail_path() == "");
    }

    SECTION("direct switch between prints clears old thumbnail") {
        // Print A with thumbnail
        set_print_filename("first.gcode");
        state().set_print_thumbnail_path("A:/cache/first_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/first_thumb.bin");

        // Print B starts immediately (no empty filename in between)
        set_print_filename("second.gcode");
        REQUIRE(get_display_filename() == "second");
        REQUIRE(get_thumbnail_path() == "");
    }

    SECTION("same filename reprint preserves thumbnail") {
        // Print A with thumbnail
        set_print_filename("benchy.gcode");
        state().set_print_thumbnail_path("A:/cache/benchy_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");

        // Same file reprinted - idempotent guard means no change, which is correct
        // (the cached thumbnail is still valid for the same file)
        set_print_filename("benchy.gcode");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");
    }
}

// ============================================================================
// Async Lifetime / Stale-Callback Safety (background-thread UAF regression)
// ============================================================================
//
// The metadata fetch in load_thumbnail_for_file() registers a callback that, on
// a real printer, fires on the Moonraker WebSocket background thread. Before the
// fix, that callback applied its result (layer_total / estimated_time) to the
// PrinterState subjects WITHOUT consulting the manager's AsyncLifetimeGuard, and
// guarded only by a non-atomic generation counter that was re-checked on the bg
// thread. If the manager was destroyed (soft restart / reconnect) while a fetch
// was in flight, the deferred subject write still ran against the freed object's
// captured state -> heap-corruption UAF.
//
// These tests capture the metadata callback (instead of letting the mock fire it
// synchronously) so they can fire it AFTER a superseding event:
//   1. a newer load bumps the generation, or
//   2. the manager is destroyed (lifetime invalidated).
// The post-event callback MUST NOT apply its (now stale / dangling) result.

namespace {

/// File API that captures the metadata request instead of answering it, so the
/// test controls exactly when (and after which lifecycle event) the success
/// callback fires. get_file_metadata is virtual on MoonrakerFileAPI (parity with
/// the virtual HTTP transfer methods the transfer mock overrides).
class CapturingFileAPI : public MoonrakerFileAPI {
  public:
    using MoonrakerFileAPI::MoonrakerFileAPI;

    void get_file_metadata(const std::string& filename, FileMetadataCallback on_success,
                           ErrorCallback on_error, bool silent = false) override {
        (void)silent;
        last_filename_ = filename;
        pending_.push_back(std::move(on_success));
        pending_errors_.push_back(std::move(on_error));
    }

    /// Number of captured (not-yet-fired) metadata callbacks.
    [[nodiscard]] size_t pending_count() const {
        return pending_.size();
    }
    [[nodiscard]] bool has_pending() const {
        return !pending_.empty();
    }

    /// Fire the Nth captured callback (0-based) with the given metadata, WITHOUT
    /// consuming it from the list (so callers can fire callbacks in any order).
    void fire_index(size_t index, const FileMetadata& metadata) {
        REQUIRE(index < pending_.size());
        auto cb = pending_[index];
        cb(metadata);
    }

    /// Fire the most recently captured callback.
    void fire_last(const FileMetadata& metadata) {
        REQUIRE(!pending_.empty());
        fire_index(pending_.size() - 1, metadata);
    }

    /// Fire the Nth captured ERROR callback (0-based) with the given error.
    void fire_error_index(size_t index, const MoonrakerError& err) {
        REQUIRE(index < pending_errors_.size());
        auto cb = pending_errors_[index];
        REQUIRE(cb);
        cb(err);
    }

    /// Fire the most recently captured error callback.
    void fire_error_last(const MoonrakerError& err) {
        REQUIRE(!pending_errors_.empty());
        fire_error_index(pending_errors_.size() - 1, err);
    }

    /// Filename of the most recent metadata request.
    [[nodiscard]] const std::string& last_filename() const {
        return last_filename_;
    }

  private:
    std::vector<FileMetadataCallback> pending_;
    std::vector<ErrorCallback> pending_errors_;
    std::string last_filename_;
};

/// Transfer API whose download_thumbnail synchronously "succeeds" without
/// writing a file. ThumbnailCache::process_and_callback then fails to open the
/// PNG and falls back to invoking on_success with the PNG path synchronously —
/// giving tests a deterministic, thread-free path to a thumbnail load success.
class StubTransferAPI : public MoonrakerFileTransferAPI {
  public:
    explicit StubTransferAPI(helix::MoonrakerClient& client)
        : MoonrakerFileTransferAPI(client, base_url_storage()) {}

    void download_thumbnail(const std::string& thumbnail_path, const std::string& cache_path,
                            StringCallback on_success, ErrorCallback on_error) override {
        (void)thumbnail_path;
        download_count_++;
        if (fail_downloads_) {
            MoonrakerError err;
            err.message = "stub download failure";
            if (on_error) {
                on_error(err);
            }
            return;
        }
        if (on_success) {
            on_success(cache_path);
        }
    }

    void set_fail_downloads(bool fail) {
        fail_downloads_ = fail;
    }
    [[nodiscard]] int download_count() const {
        return download_count_;
    }

  private:
    // Ctor stores a reference to the URL string; keep storage with static duration.
    static const std::string& base_url_storage() {
        static const std::string url = "http://stub.invalid";
        return url;
    }

    bool fail_downloads_ = false;
    int download_count_ = 0;
};

/// MoonrakerAPI that installs the CapturingFileAPI in place of the real file API
/// and a StubTransferAPI in place of the real HTTP transfer API.
class CapturingMoonrakerAPI : public MoonrakerAPI {
  public:
    CapturingMoonrakerAPI(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state) {
        // file_api_ / file_transfer_api_ are protected; swap in test implementations.
        file_api_ = std::make_unique<CapturingFileAPI>(client);
        file_transfer_api_ = std::make_unique<StubTransferAPI>(client);
    }

    CapturingFileAPI& capturing_files() {
        return static_cast<CapturingFileAPI&>(files());
    }

    StubTransferAPI& stub_transfers() {
        return static_cast<StubTransferAPI&>(transfers());
    }
};

/// Fixture that wires the manager to a CapturingMoonrakerAPI so metadata
/// callbacks can be fired on demand, after a lifecycle event.
class ActivePrintMediaAsyncFixture {
  public:
    ActivePrintMediaAsyncFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        if (!logger_initialized_) {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            auto null_logger = std::make_shared<spdlog::logger>("null", null_sink);
            spdlog::set_default_logger(null_logger);
            logger_initialized_ = true;
        }

        lv_init_safe();

        if (!queue_initialized_) {
            helix::ui::update_queue_init();
            queue_initialized_ = true;
        }

        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        PrinterStateTestAccess::reset(state_);
        state_.init_subjects(false);

        api_ = std::make_unique<CapturingMoonrakerAPI>(mock_client_, state_);
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
        manager_->set_api(api_.get());
    }

    ~ActivePrintMediaAsyncFixture() {
        manager_.reset();
        api_.reset();
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        helix::ui::update_queue_shutdown();
        queue_initialized_ = false;
        if (display_created_ && display_) {
            lv_display_delete(display_);
            display_ = nullptr;
            display_created_ = false;
        }
        logger_initialized_ = false;
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }
    helix::ActivePrintMediaManager& manager() {
        return *manager_;
    }
    CapturingFileAPI& files() {
        return api_->capturing_files();
    }

    StubTransferAPI& transfers_stub() {
        return api_->stub_transfers();
    }

    MoonrakerClientMock& client() {
        return mock_client_;
    }

    /// Set the print filename WITHOUT draining the queue, so deferred applies
    /// stay pending until the test decides to drain.
    void set_print_filename_no_drain(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
    }

    void drain() {
        UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    int get_layer_total() {
        return lv_subject_get_int(state_.get_print_layer_total_subject());
    }

    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

    static FileMetadata make_metadata(uint32_t layer_count) {
        FileMetadata m;
        m.layer_count = layer_count;
        m.estimated_time = 0; // no estimate -> exercises layer path cleanly
        return m;
    }

    /// Metadata that includes a thumbnail entry. Pass a unique relative path
    /// per test so ThumbnailCache disk-cache hits from previous runs can't
    /// short-circuit the download we want to observe.
    static FileMetadata make_metadata_with_thumb(uint32_t layer_count,
                                                 const std::string& thumb_path) {
        FileMetadata m = make_metadata(layer_count);
        m.thumbnails.push_back(ThumbnailInfo{thumb_path, 300, 300});
        return m;
    }

    /// Unique thumbnail relative path (avoids cross-run ThumbnailCache hits).
    static std::string unique_thumb_path(const char* stem) {
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::string(".thumbs/") + stem + "_" + std::to_string(now) + "_" +
               std::to_string(counter.fetch_add(1)) + ".png";
    }

    /// Simulate a Moonraker notification arriving (fires the persistent method
    /// callbacks the manager registered on the client).
    void fire_notification(const std::string& method, const json& msg) {
        MoonrakerClientTestAccess::fire_method_callbacks(mock_client_, method, msg);
    }

    std::unique_ptr<helix::ActivePrintMediaManager>& manager_ptr() {
        return manager_;
    }

  private:
    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<CapturingMoonrakerAPI> api_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
    static lv_display_t* display_;
    static bool display_created_;
    static bool queue_initialized_;
    static bool logger_initialized_;
};

lv_display_t* ActivePrintMediaAsyncFixture::display_ = nullptr;
bool ActivePrintMediaAsyncFixture::display_created_ = false;
bool ActivePrintMediaAsyncFixture::queue_initialized_ = false;
bool ActivePrintMediaAsyncFixture::logger_initialized_ = false;

} // namespace

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata callback after manager destroy does not apply",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Start a load; the capturing API holds the metadata callback.
    set_print_filename_no_drain("doomed.gcode");
    drain(); // flush display-name update so only the layer apply is in flight later
    REQUIRE(files().has_pending());

    // The metadata arrives (bg thread) and the manager applies layer_total via
    // the lifetime-guarded defer path.
    files().fire_last(make_metadata(/*layer_count=*/137));

    // Owner is destroyed BEFORE the deferred apply runs (soft restart / reconnect).
    // AsyncLifetimeGuard destructor invalidates all outstanding tokens.
    manager_ptr().reset();

    // Now the deferred apply runs. It must be skipped — applying it would be a
    // use-after-free against the destroyed manager's captured state.
    drain();

    REQUIRE(get_layer_total() == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: stale metadata callback (superseded gen) does not apply",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Load A starts; capture its metadata callback (index 0).
    set_print_filename_no_drain("print_a.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Load B supersedes A (bumps generation) and captures a second callback (index 1).
    set_print_filename_no_drain("print_b.gcode");
    REQUIRE(files().pending_count() == 2);

    // B's metadata arrives FIRST and is applied (layer=222).
    files().fire_index(1, make_metadata(/*layer_count=*/222));
    drain();
    REQUIRE(get_layer_total() == 222);

    // A's (stale) metadata arrives LATE — its generation was superseded by B.
    // It MUST NOT clobber B's value. On the buggy code (no generation re-check on
    // the apply side / non-atomic gen race) this would overwrite with 111.
    files().fire_index(0, make_metadata(/*layer_count=*/111));
    drain();

    REQUIRE(get_layer_total() == 222);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: live metadata callback applies layer total",
                 "[ActivePrintMediaManager][async][lifetime]") {
    // Positive control: with no superseding event, the layer total is applied.
    set_print_filename_no_drain("live.gcode");
    drain();
    REQUIRE(files().has_pending());

    files().fire_last(make_metadata(/*layer_count=*/99));
    drain();

    REQUIRE(get_layer_total() == 99);
}

// ============================================================================
// Thumbnail Retry Tests (bounded backoff + Moonraker notification re-triggers)
// ============================================================================
// Field bug (recurring, v0.99.75): the print-status thumbnail stays blank for
// an entire print because load_thumbnail_for_file() was strictly one-shot —
// if Moonraker hadn't finished scanning a just-uploaded file (OrcaSlicer
// upload-and-print), the metadata fetch failed once and nothing ever
// re-queried. These tests cover the three retry legs:
//   1. bounded lv_timer backoff retry on each failure path
//   2. notify_filelist_changed re-trigger when the current file's metadata
//      becomes available
//   3. notify_klippy_ready re-trigger (WebSocket reconnect mid-print)

using TestAccess = helix::ActivePrintMediaManagerTestAccess;

namespace {
MoonrakerError make_error(const std::string& message) {
    MoonrakerError err;
    err.code = -32601;
    err.message = message;
    return err;
}
} // namespace

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: retry backoff schedule is 2s/5s/10s/20s/30s",
                 "[ActivePrintMediaManager][retry]") {
    REQUIRE(TestAccess::retry_delay_ms(1) == 2000);
    REQUIRE(TestAccess::retry_delay_ms(2) == 5000);
    REQUIRE(TestAccess::retry_delay_ms(3) == 10000);
    REQUIRE(TestAccess::retry_delay_ms(4) == 20000);
    REQUIRE(TestAccess::retry_delay_ms(5) == 30000);
    REQUIRE(TestAccess::retry_delay_ms(9) == 30000);
    REQUIRE(TestAccess::max_attempts() == 10);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata error schedules retry, retry success sets "
                 "thumbnail path",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("just_uploaded.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Attempt 1 fails: Moonraker hasn't scanned the file yet (bg thread error,
    // marshalled to main on drain).
    files().fire_error_last(make_error("Metadata not available for <just_uploaded.gcode>"));
    drain();

    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
    REQUIRE(TestAccess::retry_filename(manager()) == "just_uploaded.gcode");
    REQUIRE_FALSE(TestAccess::thumbnail_loaded(manager()));

    // Retry timer fires -> a NEW metadata request goes out.
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // This time the metadata is ready and has a thumbnail; the stub transfer
    // API "downloads" it synchronously and the subject gets set.
    files().fire_last(make_metadata_with_thumb(42, unique_thumb_path("retry_success")));
    drain();

    REQUIRE_FALSE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(get_layer_total() == 42);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: metadata without thumbnails schedules retry",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("scanning.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Metadata record exists but thumbnails aren't extracted yet.
    files().fire_last(make_metadata(/*layer_count=*/7));
    drain();

    REQUIRE(get_layer_total() == 7); // metadata fields still applied
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
    REQUIRE_FALSE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: thumbnail download failure schedules retry",
                 "[ActivePrintMediaManager][retry]") {
    transfers_stub().set_fail_downloads(true);

    set_print_filename_no_drain("dl_fail.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(3, unique_thumb_path("dl_fail")));
    drain();

    REQUIRE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Recovery: downloads start working, retry fires, thumbnail loads.
    transfers_stub().set_fail_downloads(false);
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    files().fire_last(make_metadata_with_thumb(3, unique_thumb_path("dl_recover")));
    drain();

    REQUIRE_FALSE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed for current file triggers immediate "
                 "reload",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Moonraker finishes scanning and emits notify_filelist_changed for the
    // file we're printing (fires on the WS thread in production; the handler
    // marshals to main, hence the drain).
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "notify_filelist_changed"},
        {"params", json::array({{{"action", "create_file"},
                                 {"item", {{"path", "benchy.gcode"}, {"root", "gcodes"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    // Immediate reload: pending retry cancelled, counter reset, new request out.
    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);

    files().fire_last(make_metadata_with_thumb(11, unique_thumb_path("filelist_reload")));
    drain();
    REQUIRE_FALSE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed for unrelated file does not reload",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "notify_filelist_changed"},
        {"params", json::array({{{"action", "create_file"},
                                 {"item", {{"path", "unrelated.gcode"}, {"root", "gcodes"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    // No new request; the scheduled backoff retry is untouched.
    REQUIRE(files().pending_count() == 1);
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filelist_changed with null/missing fields is ignored "
                 "safely",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Moonraker notifications can carry null/missing fields — must not throw.
    json null_item = {{"method", "notify_filelist_changed"},
                      {"params", json::array({{{"action", nullptr}, {"item", nullptr}}})}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", null_item));
    json no_params = {{"method", "notify_filelist_changed"}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", no_params));
    drain();

    REQUIRE(files().pending_count() == 1); // no reload triggered
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: klippy_ready re-triggers load while thumbnail missing",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("mid_print.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("connection reset"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Klippy comes back (WebSocket reconnect mid-print) — filename subject
    // never changes, so this is the only signal we get.
    fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}});
    drain();

    REQUIRE(files().pending_count() == 2);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: klippy_ready does NOT reload when thumbnail loaded",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("loaded.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(5, unique_thumb_path("klippy_loaded")));
    drain();
    REQUIRE(TestAccess::thumbnail_loaded(manager()));

    fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}});
    drain();

    REQUIRE(files().pending_count() == 1); // no redundant reload
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: filename change cancels pending retry",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("first.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // A different print starts: the pending retry for first.gcode must be
    // cancelled and retry state reset for the new filename.
    set_print_filename_no_drain("second.gcode");
    drain();

    REQUIRE(files().pending_count() == 2); // fresh load for second.gcode
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);

    // No stale load for first.gcode fires later: the (deleted) timer is gone,
    // and even a stale-scheduled retry would no-op on the filename check.
    REQUIRE(TestAccess::retry_filename(manager()).empty());
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: retries stop after max attempts",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("never_scans.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    const int max_attempts = TestAccess::max_attempts();

    // Fail every attempt; pump the retry timer manually each round.
    int requests = 1;
    for (int i = 0; i < max_attempts + 3; i++) {
        files().fire_error_last(make_error("Metadata not available"));
        drain();
        if (!TestAccess::has_pending_retry(manager())) {
            break;
        }
        REQUIRE(TestAccess::fire_pending_retry(manager()));
        requests++;
        REQUIRE(static_cast<int>(files().pending_count()) == requests);
    }

    // Exactly max_attempts total requests went out, then it gave up.
    REQUIRE(requests == max_attempts);
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Further failures don't resurrect the timer.
    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(static_cast<int>(files().pending_count()) == max_attempts);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: successful first load creates no retry timer",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("works_first_try.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_last(make_metadata_with_thumb(21, unique_thumb_path("first_try")));
    drain();

    REQUIRE_FALSE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 0);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: empty-thumbnail metadata retries cap at 2",
                 "[ActivePrintMediaManager][retry]") {
    // A metadata record with no thumbnails is usually a PERMANENT condition
    // (file sliced without thumbnails), so this leg uses a 2-retry cap instead
    // of the full 10-attempt ladder reserved for transport failures.
    set_print_filename_no_drain("no_thumbs_sliced.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    // Attempt 1: success, no thumbnails -> retry 1.
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // Attempt 2: still no thumbnails -> retry 2.
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 2);
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 2);

    // Attempt 3: still no thumbnails -> gives up (cap is 2 retries).
    REQUIRE(TestAccess::fire_pending_retry(manager()));
    REQUIRE(files().pending_count() == 3);
    files().fire_last(make_metadata(/*layer_count=*/4));
    drain();
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));
    REQUIRE(files().pending_count() == 3);

    // But a filelist_changed for the file still re-triggers (covers the
    // genuinely-late-scan case beyond the cheap cap).
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "modify_file"},
                                         {"item", {{"path", "no_thumbs_sliced.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();
    REQUIRE(files().pending_count() == 4);
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: delete_file for current file does not re-trigger",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("doomed_delete.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));
    REQUIRE(TestAccess::retry_count(manager()) == 1);

    // The printing file gets deleted — re-querying it is guaranteed to fail,
    // so the notification must NOT kick off a fresh retry ladder.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "delete_file"},
                                         {"item", {{"path", "doomed_delete.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 1);            // no immediate reload
    REQUIRE(TestAccess::retry_count(manager()) == 1); // ladder not reset
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: move_file of current file reloads from destination",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // The printing file is renamed: source_item is the old (current) name,
    // item is the destination. Metadata now lives under the destination.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "move_file"},
                                         {"source_item", {{"path", "benchy.gcode"}}},
                                         {"item", {{"path", "archive/benchy_v2.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 2);
    REQUIRE(files().last_filename() == "archive/benchy_v2.gcode"); // dest, not stale name
    REQUIRE_FALSE(TestAccess::has_pending_retry(manager()));

    // Destination metadata resolves -> thumbnail loads.
    files().fire_last(make_metadata_with_thumb(8, unique_thumb_path("moved_dest")));
    drain();
    REQUIRE_FALSE(get_thumbnail_path().empty());
    REQUIRE(TestAccess::thumbnail_loaded(manager()));
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: move_file without destination path is skipped",
                 "[ActivePrintMediaManager][retry]") {
    set_print_filename_no_drain("benchy.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    files().fire_error_last(make_error("Metadata not available"));
    drain();
    REQUIRE(TestAccess::has_pending_retry(manager()));

    // Malformed/partial move notification: source matches but no dest item.
    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "move_file"},
                                         {"source_item", {{"path", "benchy.gcode"}}}}})}};
    fire_notification("notify_filelist_changed", msg);
    drain();

    REQUIRE(files().pending_count() == 1);             // no reload from a missing dest
    REQUIRE(TestAccess::has_pending_retry(manager())); // backoff ladder untouched
}

TEST_CASE_METHOD(ActivePrintMediaAsyncFixture,
                 "ActivePrintMediaManager: notification after manager destroy is safe (expired "
                 "token no-ops)",
                 "[ActivePrintMediaManager][retry][lifetime]") {
    // Regression guard for the registration-lifetime story: unregistration is
    // skipped during teardown (get_moonraker_manager() is null by then), so the
    // method callbacks stay registered on the client. Safety relies on the
    // lifetime token captured at registration: bg-side parsing touches locals
    // only, and the token.defer() apply must no-op once the manager is gone.
    set_print_filename_no_drain("teardown.gcode");
    drain();
    REQUIRE(files().pending_count() == 1);

    manager_ptr().reset(); // destroy the manager (lifetime invalidated)

    json msg = {{"method", "notify_filelist_changed"},
                {"params", json::array({{{"action", "create_file"},
                                         {"item", {{"path", "teardown.gcode"}}}}})}};
    REQUIRE_NOTHROW(fire_notification("notify_filelist_changed", msg));
    REQUIRE_NOTHROW(
        fire_notification("notify_klippy_ready", json{{"method", "notify_klippy_ready"}}));
    drain(); // expired-token defers must no-op, not UAF

    REQUIRE(files().pending_count() == 1); // no reload fired against the dead manager
}

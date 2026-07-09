// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <atomic>
#include <memory>
#include <string>

namespace helix {

/**
 * @brief Manages display info for the active print (thumbnail, display filename)
 *
 * Decouples shared print media from PrintStatusPanel so that:
 * 1. HomePanel always has current data (regardless of which panels are open)
 * 2. Thread-safe LVGL updates via helix::ui::queue_update()
 * 3. Single point of truth for filename resolution and thumbnail loading
 *
 * Thread Safety:
 * - set_api() must be called from main thread only
 * - set_thumbnail_source() must be called from main thread only
 * - Observer callbacks from PrinterState trigger on main thread (LVGL observer)
 * - All lv_subject updates are deferred to main thread via helix::ui::queue_update()
 *
 * Initialization order: PrinterState -> ActivePrintMediaManager -> Panels
 */
class ActivePrintMediaManager {
  public:
    explicit ActivePrintMediaManager(PrinterState& printer_state);
    ~ActivePrintMediaManager();

    // Non-copyable
    ActivePrintMediaManager(const ActivePrintMediaManager&) = delete;
    ActivePrintMediaManager& operator=(const ActivePrintMediaManager&) = delete;

    /**
     * @brief Set the MoonrakerAPI instance for thumbnail downloads
     *
     * Must be called before thumbnail loading will work. Also registers the
     * persistent Moonraker method callbacks (notify_filelist_changed /
     * notify_klippy_ready) used to re-trigger thumbnail loads that failed
     * because Moonraker hadn't finished scanning the file yet.
     *
     * @param api Pointer to MoonrakerAPI (can be nullptr to disable)
     */
    void set_api(MoonrakerAPI* api);

    /**
     * @brief Set the original filename for thumbnail lookup
     *
     * Call this when starting a print with a modified temp file to override
     * the filename used for metadata/thumbnail lookup. This handles the case
     * where Moonraker reports .helix_temp/modified_* but thumbnails are stored
     * under the original filename.
     *
     * @param original_filename The original filename before modification
     */
    void set_thumbnail_source(const std::string& original_filename);

    /**
     * @brief Clear the thumbnail source override
     *
     * Called when print ends to reset state for next print.
     */
    void clear_thumbnail_source();

    /**
     * @brief Set the thumbnail path directly (bypasses Moonraker API lookup)
     *
     * Call this when starting a print with a pre-extracted thumbnail
     * (e.g., from USB drive or embedded in G-code). This sets the thumbnail
     * path subject directly without going through the Moonraker metadata API.
     *
     * @param path Path to the thumbnail file (e.g., "/tmp/helix/thumbnails/extracted.png")
     */
    void set_thumbnail_path(const std::string& path);

  private:
    void process_filename(const char* raw_filename);
    void load_thumbnail_for_file(const std::string& filename);
    void clear_print_info();

    // --- Bounded thumbnail retry (metadata/thumbnail fetch failures) ---
    // Moonraker may not have finished scanning a just-uploaded file when the
    // print starts (OrcaSlicer upload-and-print), so the first metadata query
    // can fail or return no thumbnails. Each failure schedules a one-shot
    // lv_timer retry with backoff (2s, 5s, 10s, 20s, then 30s) up to
    // kMaxThumbnailAttempts total attempts per filename. If the print ends
    // (empty filename) while a retry is pending, last_effective_filename_ is
    // intentionally preserved (see process_filename), so the retry may still
    // late-fill the preserved display info — intended, and bounded by the cap.

    /// Backoff delay for the given retry number (1-based: first retry = 1).
    static uint32_t retry_delay_ms(int retry_number);
    /// Schedule a retry for @p filename (main thread only). No-ops if the
    /// filename is no longer current, a retry is already pending, or
    /// @p max_retries retries have already been scheduled.
    void schedule_thumbnail_retry(const std::string& filename,
                                  int max_retries = kMaxThumbnailAttempts - 1);
    /// Cancel any pending retry timer and clear retry bookkeeping filename.
    void cancel_thumbnail_retry();
    /// Body of the retry timer: re-validates filename + generation, then reloads.
    void on_retry_timer_fired();
    static void retry_timer_cb(lv_timer_t* timer);

    // --- External re-trigger (Moonraker notifications) ---
    void register_moonraker_listeners();
    void unregister_moonraker_listeners();
    /// Main-thread handler for notify_filelist_changed (fields pre-parsed on
    /// the WebSocket thread into plain strings).
    void handle_filelist_changed(const std::string& action, const std::string& item_path,
                                 const std::string& source_path);
    /// Reset retry state and reload the thumbnail for the current filename
    /// if one is set and the thumbnail hasn't successfully loaded yet.
    void retrigger_thumbnail_load(const char* reason);

    PrinterState& printer_state_;
    MoonrakerAPI* api_ = nullptr;
    ObserverGuard print_filename_observer_;
    std::string thumbnail_source_filename_;
    std::string last_effective_filename_;
    std::string last_loaded_thumbnail_filename_;
    bool last_was_empty_ = false; ///< Prevents repeated "empty filename" log spam

    /// Max total metadata/thumbnail load attempts per filename (1 initial + 9 retries).
    static constexpr int kMaxThumbnailAttempts = 10;

    /// Lower retry cap for the success-with-empty-thumbnails leg: a metadata
    /// record can briefly lack thumbnails mid-scan, but the common cause is a
    /// file sliced WITHOUT thumbnails — a permanent condition where the full
    /// ladder would just burn RPCs. Late-scan cases beyond this are covered by
    /// the notify_filelist_changed / notify_klippy_ready re-triggers.
    static constexpr int kMaxEmptyThumbnailRetries = 2;

    helix::ui::LvglTimerGuard retry_timer_; ///< Pending one-shot retry (empty when none)
    int thumbnail_retry_count_ = 0;         ///< Retries scheduled for the current filename
    std::string retry_filename_;            ///< Filename the pending retry is for
    uint32_t retry_generation_ = 0;         ///< Load generation the pending retry belongs to
    bool thumbnail_loaded_ = false;         ///< Thumbnail successfully loaded for current filename

    MoonrakerAPI* listener_api_ = nullptr; ///< API the method callbacks are registered on
    std::string filelist_handler_name_;
    std::string klippy_ready_handler_name_;

    /// Generation counter for stale-callback detection. Bumped on the main thread
    /// each time a new load starts; read on the main thread inside deferred applies.
    /// Atomic because the value is captured by background-thread callbacks (even
    /// though the re-check itself always runs on the main thread via tok.defer).
    std::atomic<uint32_t> thumbnail_load_generation_{0};

    /// Async callback safety guard. Invalidated on destruction, so any in-flight
    /// background-thread callback that marshals back via tok.defer() no-ops if the
    /// manager has been torn down (soft restart / reconnect).
    helix::AsyncLifetimeGuard lifetime_;

    friend class ActivePrintMediaManagerTestAccess;
};

/**
 * @brief Initialize the global ActivePrintMediaManager singleton
 *
 * Must be called after init_printer_state_subjects() and before panels
 * that depend on print_display_filename/print_thumbnail_path subjects.
 */
void init_active_print_media_manager();

/**
 * @brief Destroy the ActivePrintMediaManager singleton for soft restart
 *
 * Releases the observer guard and destroys the instance. Must be called
 * BEFORE PrinterState::deinit_subjects() to avoid dangling observer pointers.
 */
void deinit_active_print_media_manager();

/**
 * @brief Get the global ActivePrintMediaManager instance
 *
 * @return Reference to the singleton instance
 * @throws std::runtime_error if called before init_active_print_media_manager()
 */
ActivePrintMediaManager& get_active_print_media_manager();

} // namespace helix

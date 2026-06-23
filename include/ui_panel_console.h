// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "console_filter_engine.h"
#include "lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <deque>
#include <memory>
#include <string>

#include "hv/json.hpp"

/**
 * @file ui_panel_console.h
 * @brief G-code console panel with command history and real-time streaming
 *
 * Full-featured G-code console overlay for interacting with Klipper via
 * Moonraker. Displays color-coded command history with real-time streaming
 * of G-code responses.
 *
 * ## Features
 * - Command history display from Moonraker gcode_store
 * - Real-time response streaming via notify_gcode_response WebSocket
 * - G-code input field with Enter key submission
 * - Command history navigation (Up/Down arrow keys)
 * - Color-coded output (commands, responses green, errors red)
 * - HTML span parsing for AFC/Happy Hare colored output
 * - Temperature message filtering (periodic T:/B: reports)
 * - Auto-scroll to newest messages (pauses when user scrolls up)
 * - Empty state when no history available
 *
 * ## Moonraker API
 * - server.gcode_store - Fetch command history
 * - notify_gcode_response - Real-time response subscription
 * - printer.gcode.script - Send G-code commands
 *
 * @see docs/FEATURE_STATUS.md for implementation progress
 */
class ConsolePanel : public OverlayBase {
  public:
    ConsolePanel();
    ~ConsolePanel() override;

    // === OverlayBase interface ===
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    [[nodiscard]] const char* get_name() const override {
        return "Console";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivate() override;
    void on_ui_destroyed() override;

    /// Send the current G-code command from the input field via Moonraker
    void send_gcode_command();

    /// Clear all entries from the console display and show empty state
    void clear_display();

  private:
    struct GcodeEntry {
        std::string message;    ///< The G-code command or response text
        double timestamp = 0.0; ///< Unix timestamp from Moonraker
        enum class Type {
            COMMAND, ///< User-entered G-code command
            RESPONSE ///< Klipper response (ok, error, info)
        } type = Type::COMMAND;
        bool is_error = false; ///< True if response contains error (!! prefix)
    };

    /// Fetch initial history from Moonraker's server.gcode_store
    void fetch_history();

    /// Replace all displayed entries with the given history (oldest first)
    void populate_entries(const std::vector<GcodeEntry>& entries);

    /// Create a single color-coded console line widget for an entry
    void create_entry_widget(const GcodeEntry& entry);

    /// Remove all entries and child widgets from the container
    void clear_entries();

    /// Scroll to the bottom (newest entries visible)
    void scroll_to_bottom();

    /// True if message starts with "!!" or "Error" (case-insensitive)
    static bool is_error_message(const std::string& message);

    /// Toggle console_container_ vs empty_state_ visibility
    void update_visibility();

    /// Append a single entry, create its widget, and auto-scroll if appropriate
    void add_entry(const GcodeEntry& entry);

    /// Handle incoming notify_gcode_response WebSocket notification
    void on_gcode_response(const nlohmann::json& msg);

    /// Subscribe to real-time G-code responses (called from on_activate)
    void subscribe_to_gcode_responses();

    /// Unsubscribe from real-time G-code responses (called from on_deactivate)
    void unsubscribe_from_gcode_responses();

    /// Rebuild the firmware-noise filter engine for the current printer.
    /// Cheap when the printer hasn't changed (compares against firmware_filter_printer_).
    void rebuild_firmware_filter();

    /// True if message is a periodic temperature report (e.g. "ok T:210.0 /210.0 B:60.0 /60.0")
    static bool is_temp_message(const std::string& message);

    // Widget references
    lv_obj_t* console_container_ = nullptr; ///< Scrollable container for entries
    lv_obj_t* empty_state_ = nullptr;       ///< Shown when no entries
    lv_obj_t* status_label_ = nullptr;      ///< Status message label
    lv_obj_t* gcode_input_ = nullptr;       ///< G-code text input field

    // Data
    std::deque<GcodeEntry> entries_;           ///< History buffer
    static constexpr size_t MAX_ENTRIES = 200; ///< Maximum entries to display
    static constexpr int FETCH_COUNT = 100;    ///< Number of entries to fetch

    // Command history (up/down arrow navigation)
    std::deque<std::string> command_history_; ///< Previously sent commands (newest first)
    int history_index_ = -1;                  ///< -1 = not browsing, 0 = most recent
    std::string saved_input_;                 ///< In-progress text saved when browsing history
    static constexpr size_t MAX_HISTORY = 20; ///< Maximum commands to remember

    // Real-time subscription state
    std::string gcode_handler_name_; ///< Unique handler name for callback registration
    bool is_subscribed_ = false;     ///< True if subscribed to notify_gcode_response
    bool fetch_in_flight_ = false;   ///< True while a gcode_store fetch is pending
    bool user_scrolled_up_ = false;  ///< True if user manually scrolled up

    // Filtering — engine + observers driven by SettingsManager subjects.
    // The engine is rebuilt on every on_activate() so user pattern edits take
    // effect immediately when returning from the settings overlay.
    helix::ui::ConsoleFilterEngine firmware_filter_;
    ObserverGuard filter_temps_observer_;
    ObserverGuard filter_firmware_observer_;
    bool filter_temps_ = true;
    bool filter_firmware_noise_ = true;

    // Timestamp display (responsive: medium+ breakpoints only)
    bool show_timestamps_ = false; ///< True if screen is large enough for timestamps

    // Subjects
    SubjectManager subjects_;
    char status_buf_[128] = {};
    lv_subject_t status_subject_{};
    lv_subject_t status_visible_subject_{}; ///< 1 = status text visible, 0 = hidden
    lv_subject_t has_entries_subject_{};    ///< 1 = has console entries, 0 = empty

    // Callback registration tracking
    bool callbacks_registered_ = false;
};

/**
 * @brief Get global ConsolePanel instance
 * @return Reference to the singleton panel
 *
 * Creates the instance on first call. Used by static callbacks.
 */
ConsolePanel& get_global_console_panel();

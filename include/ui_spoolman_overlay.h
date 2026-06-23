// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_spoolman_overlay.h
 * @brief Spoolman settings overlay
 *
 * This overlay allows users to configure Spoolman integration settings:
 * - Enable/disable automatic weight sync
 * - Configure polling refresh interval
 *
 * Settings are persisted in Moonraker database under "helix-screen" namespace.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "overlay_base.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declarations
class MoonrakerAPI;

namespace helix::ui {

/**
 * @class SpoolmanOverlay
 * @brief Overlay for configuring Spoolman integration settings
 *
 * This overlay provides settings for Spoolman weight synchronization:
 * - Sync toggle: Enable/disable automatic polling
 * - Refresh interval: How often to poll for weight updates (30s, 60s, 120s, 300s)
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::ui::get_spoolman_overlay();
 * if (!overlay.are_subjects_initialized()) {
 *     overlay.init_subjects();
 *     overlay.register_callbacks();
 * }
 * overlay.show(parent_screen);
 * @endcode
 */
class SpoolmanOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    SpoolmanOverlay();

    /**
     * @brief Destructor
     */
    ~SpoolmanOverlay() override;

    // Non-copyable
    SpoolmanOverlay(const SpoolmanOverlay&) = delete;
    SpoolmanOverlay& operator=(const SpoolmanOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects for reactive binding
     *
     * Registers subjects for:
     * - ams_spoolman_sync_enabled: Whether sync is enabled (0/1)
     * - ams_spoolman_refresh_interval: Polling interval in seconds
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for toggle and dropdown changes.
     */
    void register_callbacks() override;

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Spoolman"
     */
    const char* get_name() const override {
        return "Spoolman";
    }

    /**
     * @brief Null widget pointers after destroy-on-close
     */
    void on_ui_destroyed() override;

    /**
     * @brief Refresh UI state when overlay becomes visible
     *
     * Re-syncs the Barcode Scanner row description subject from SettingsManager
     * so that changes made in the scanner settings sub-overlay are reflected
     * when the user returns here.
     */
    void on_activate() override;

    //
    // === Public API ===
    //

    /**
     * @brief Show the overlay
     *
     * This method:
     * 1. Ensures overlay is created (lazy init)
     * 2. Loads current settings from Moonraker database
     * 3. Updates subject values
     * 4. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    /**
     * @brief Refresh settings from Moonraker database
     *
     * Re-loads current values from the database and updates UI.
     */
    void refresh();

    /**
     * @brief Set MoonrakerAPI for database access
     *
     * @param api MoonrakerAPI instance (not owned)
     */
    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Load settings from Moonraker database
     *
     * Queries helix-screen namespace for:
     * - ams_spoolman_sync_enabled
     * - ams_weight_refresh_interval
     */
    void load_from_database();

    /**
     * @brief Save sync enabled setting to database
     *
     * @param enabled Whether sync is enabled
     */
    void save_sync_enabled(bool enabled);

    /**
     * @brief Save refresh interval to database
     *
     * @param interval_seconds Interval in seconds (30, 60, 120, 300)
     */
    void save_refresh_interval(int interval_seconds);

    /**
     * @brief Convert dropdown index to interval seconds
     *
     * @param index Dropdown index (0-3)
     * @return Interval in seconds
     */
    static int dropdown_index_to_seconds(int index);

    /**
     * @brief Convert interval seconds to dropdown index
     *
     * @param seconds Interval in seconds
     * @return Dropdown index (0-3)
     */
    static int seconds_to_dropdown_index(int seconds);

    /**
     * @brief Update UI controls from current subject values
     */
    void update_ui_from_subjects();

    //
    // === Static Callbacks ===
    //

    /**
     * @brief Callback for sync toggle change
     *
     * Called when user toggles the sync enable switch.
     * Saves setting to database and starts/stops polling.
     */
    static void on_sync_toggled(lv_event_t* e);

    /**
     * @brief Callback for interval dropdown change
     *
     * Called when user changes the polling interval.
     * Saves setting to database.
     */
    static void on_interval_changed(lv_event_t* e);

    //
    // === State ===
    //

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Sync toggle widget
    lv_obj_t* sync_toggle_ = nullptr;

    /// Interval dropdown widget
    lv_obj_t* interval_dropdown_ = nullptr;

    /// Subject for sync enabled state (0=disabled, 1=enabled)
    lv_subject_t sync_enabled_subject_;

    /// Subject for refresh interval in seconds
    lv_subject_t refresh_interval_subject_;

    /// MoonrakerAPI for database access (not owned)
    MoonrakerAPI* api_ = nullptr;

    /// Default values
    static constexpr bool DEFAULT_SYNC_ENABLED = true;
    static constexpr int DEFAULT_REFRESH_INTERVAL_SECONDS = 30;

#if HELIX_HAS_LABEL_PRINTER
    // Label printer sub-panel launcher
    static void on_label_printer_clicked(lv_event_t* e);
#endif

    // === Barcode Scanner Picker ===
    lv_subject_t scanner_device_status_subject_;
    char scanner_status_buf_[64] = {0};

    static void on_barcode_scanner_clicked(lv_event_t* e);
    void handle_barcode_scanner_clicked();
    void update_scanner_status_text();

    // === Server Setup Methods ===
    void probe_spoolman_server(const std::string& host, const std::string& port);
    void configure_spoolman(const std::string& host, const std::string& port);
    void finish_configure(const std::string& helix_content,
                          const std::vector<std::pair<std::string, std::string>>& entries);
    void ensure_moonraker_include();
    void restart_and_verify();
    void verify_spoolman_connected();
    void remove_spoolman_config();
    void update_server_url_display();
    void set_setup_status(const char* text, bool is_error = false);

    /**
     * @brief Toggle the Connect button's busy state
     *
     * When @p connecting is true, the button is disabled and its label swapped
     * to "Connecting...". When false, the original "Connect" label is restored
     * and the button is re-enabled. Called at the start of the probe flow and
     * from every terminal path (success, error, cancel).
     */
    void set_connecting(bool connecting);

    // === Server Setup Callbacks ===
    static void on_connect_clicked(lv_event_t* e);
    static void on_cancel_setup_clicked(lv_event_t* e);
    static void on_change_clicked(lv_event_t* e);
    static void on_remove_clicked(lv_event_t* e);

    // === Server Setup Widgets ===
    lv_obj_t* host_input_ = nullptr;
    lv_obj_t* port_input_ = nullptr;
    lv_obj_t* setup_status_text_ = nullptr;
    lv_obj_t* server_url_text_ = nullptr;
    lv_obj_t* connect_btn_ = nullptr;
    lv_obj_t* setup_card_ = nullptr;
    lv_obj_t* status_card_ = nullptr;
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton SpoolmanOverlay
 */
SpoolmanOverlay& get_spoolman_overlay();

} // namespace helix::ui

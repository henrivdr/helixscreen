// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_ams_environment_overlay.h
 * @brief AMS Environment overlay — full temperature/humidity detail and dryer controls
 *
 * Opened from the compact environment indicator on the AMS panel.
 * Shows large readouts, material comfort ranges, and dryer controls
 * (when the backend supports drying).
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Main thread only
 */

#pragma once

#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <lvgl/lvgl.h>

#include <string>
#include <vector>

// Forward declarations
class AmsBackend;
struct DryingPreset;

namespace helix::ui {

/**
 * @class AmsEnvironmentOverlay
 * @brief Full environment detail overlay with dryer controls
 *
 * Two layouts based on backend capability:
 * - Passive (no dryer): Readouts + material comfort ranges + storage advice
 * - Active (dryer): Readouts + preset/temp/duration controls + start/stop
 */
class AmsEnvironmentOverlay : public OverlayBase {
  public:
    AmsEnvironmentOverlay();
    ~AmsEnvironmentOverlay() override;

    // Non-copyable
    AmsEnvironmentOverlay(const AmsEnvironmentOverlay&) = delete;
    AmsEnvironmentOverlay& operator=(const AmsEnvironmentOverlay&) = delete;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "AMS Environment";
    }

    // === Public API ===

    /**
     * @brief Show the environment overlay for a given unit
     * @param parent_screen Parent screen for overlay creation
     * @param unit_index AMS unit index (0-based)
     */
    void show(lv_obj_t* parent_screen, int unit_index = 0);

    /**
     * @brief Refresh overlay data from backend
     */
    void refresh();

  private:
    // === Internal Methods ===

    /// Update all subjects from backend state
    void update_from_backend();

    /// Build the material comfort ranges text from current humidity
    void update_comfort_text(float humidity_pct);

    /// Populate the preset dropdown from backend presets
    void populate_presets();

    /// Apply a preset to the spinboxes
    void apply_preset(int index);

    /// Auto-select preset based on loaded materials
    void auto_select_preset();

    // === Static Callbacks ===

    static void on_start_stop_clicked(lv_event_t* e);
    static void on_preset_changed(lv_event_t* e);

    // === State ===

    /// Alias for overlay_root_ to match existing pattern
    lv_obj_t*& overlay_ = overlay_root_;

    /// Current unit index
    int unit_index_ = 0;

    /// Cached dryer presets
    std::vector<DryingPreset> cached_presets_;

    /// Widget pointers (found after create)
    lv_obj_t* preset_dropdown_ = nullptr;
    lv_obj_t* temp_input_ = nullptr;
    lv_obj_t* duration_input_ = nullptr;
    lv_obj_t* temp_range_label_ = nullptr;

    // === Subjects (managed by SubjectManager) ===

    SubjectManager subjects_;

    lv_subject_t temp_text_subject_;
    char temp_text_buf_[64] = {};

    lv_subject_t target_temp_text_subject_;
    char target_temp_text_buf_[32] = {};

    lv_subject_t humidity_text_subject_;
    char humidity_text_buf_[32] = {};

    lv_subject_t title_text_subject_;
    char title_text_buf_[64] = {};

    lv_subject_t dryer_visible_subject_;
    lv_subject_t no_dryer_visible_subject_;
    lv_subject_t drying_active_subject_;

    lv_subject_t drying_text_subject_;
    char drying_text_buf_[64] = {};

    lv_subject_t drying_progress_subject_;

    static constexpr int MAX_COMFORT_ROWS = 4;
    lv_subject_t comfort_visible_[MAX_COMFORT_ROWS] = {};
    lv_subject_t comfort_status_[MAX_COMFORT_ROWS] = {}; ///< 0=OK, 1=Marginal, 2=Too humid
    lv_subject_t comfort_text_[MAX_COMFORT_ROWS] = {};
    char comfort_text_buf_[MAX_COMFORT_ROWS][96] = {};

    lv_subject_t start_stop_text_subject_;
    char start_stop_text_buf_[32] = {};

    lv_subject_t preset_text_subject_;
    char preset_text_buf_[64] = {};
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup.
 *
 * @return Reference to singleton AmsEnvironmentOverlay
 */
AmsEnvironmentOverlay& get_ams_environment_overlay();

} // namespace helix::ui

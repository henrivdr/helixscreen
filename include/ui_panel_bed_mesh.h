// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_subscription_guard.h"

#include "async_lifetime_guard.h"
#include "moonraker_types.h" // For BedMeshProfile
#include "operation_timeout_guard.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class MoonrakerAPI;

/**
 * @brief Bed mesh visualization panel with 3D renderer
 *
 * Interactive 3D visualization of printer bed mesh height maps with touch-drag
 * rotation, color-coded height mapping, profile switching, and statistics.
 *
 * Features:
 * - Mainsail-style two-card layout (Current Mesh stats + Profiles list)
 * - Profile management: load, rename, delete, calibrate
 * - SAVE_CONFIG prompt after modifications
 *
 * @see ui_bed_mesh.h for bed mesh widget API
 */

// Maximum number of profiles displayed in UI
constexpr int BED_MESH_MAX_PROFILES = 5;

/// Calibration modal state machine
enum class BedMeshCalibrationState {
    IDLE = 0,    ///< Modal not shown
    PROBING = 1, ///< Actively probing (progress shown)
    NAMING = 2,  ///< Probing complete, awaiting profile name
    ERROR = 3    ///< Error occurred
};

class BedMeshPanel : public OverlayBase {
  public:
    BedMeshPanel();
    ~BedMeshPanel() override;

    // === OverlayBase interface ===
    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Bed Mesh Panel";
    }

    // === Lifecycle hooks ===
    void on_activate() override;
    void on_deactivate() override;
    void on_ui_destroyed() override;

    /**
     * @brief Load mesh data and render
     * @param mesh_data 2D vector of height values (row-major order)
     */
    void set_mesh_data(const std::vector<std::vector<float>>& mesh_data);

    /** @brief Force redraw of bed mesh visualization */
    void redraw();

    // Profile operations (called from XML event callbacks)
    void load_profile(int index);
    void delete_profile(int index);
    void rename_profile(int index);
    void start_calibration();

    // Modal actions
    void show_calibrate_modal();
    void show_rename_modal(const std::string& profile_name);
    void show_delete_confirm_modal(const std::string& profile_name);
    void show_save_config_modal();
    void hide_all_modals();

    // Modal callback action helpers (called from free function callbacks)
    void confirm_delete_profile();
    void decline_save_config();
    void confirm_save_config();
    void start_calibration_with_name(const std::string& profile_name);
    void confirm_rename(const std::string& new_name);

    // Calibration progress handlers (called by BedMeshProbeCollector)
    void on_probe_progress(int current, int total);
    void on_calibration_complete();
    void on_calibration_error(const std::string& message);
    void handle_emergency_stop();
    void save_profile_with_name(const std::string& name);
    void start_calibration_probing();

  private:
    void launch_calibration(MoonrakerAPI* api, int expected_probes, int probe_samples = 1);
    // ========== Subject Manager (RAII cleanup) ==========
    SubjectManager subjects_;

    // ========== Current Mesh Stats Subjects ==========
    lv_subject_t bed_mesh_available_;
    lv_subject_t bed_mesh_profile_name_;
    lv_subject_t bed_mesh_dimensions_;
    lv_subject_t bed_mesh_max_label_; // "Max [x, y]"
    lv_subject_t bed_mesh_max_value_; // "z mm"
    lv_subject_t bed_mesh_min_label_; // "Min [x, y]"
    lv_subject_t bed_mesh_min_value_; // "z mm"
    lv_subject_t bed_mesh_variance_;

    char profile_name_buf_[64];
    char dimensions_buf_[64];
    char max_label_buf_[48];
    char max_value_buf_[32];
    char min_label_buf_[48];
    char min_value_buf_[32];
    char variance_buf_[64];

    // ========== Profile List Subjects (5 profiles max) ==========
    lv_subject_t bed_mesh_profile_count_;

    std::array<lv_subject_t, BED_MESH_MAX_PROFILES> profile_name_subjects_;
    std::array<lv_subject_t, BED_MESH_MAX_PROFILES> profile_range_subjects_;
    std::array<lv_subject_t, BED_MESH_MAX_PROFILES> profile_active_subjects_;

    std::array<std::array<char, 64>, BED_MESH_MAX_PROFILES> profile_name_bufs_;
    std::array<std::array<char, 32>, BED_MESH_MAX_PROFILES> profile_range_bufs_;

    // Profile names stored for operations
    std::array<std::string, BED_MESH_MAX_PROFILES> profile_names_;

    // ========== Modal State Subjects (NOT visibility - internal state) ==========
    lv_subject_t bed_mesh_calibrating_;     // 0=idle, 1=calibrating (controls form vs spinner)
    lv_subject_t bed_mesh_rename_old_name_; // Display the old name in rename modal

    char rename_old_name_buf_[64];

    // ========== Calibration Progress Subjects ==========
    lv_subject_t bed_mesh_calibrate_state_;     ///< CalibrationState enum value
    lv_subject_t bed_mesh_probe_progress_;      ///< 0-100 percentage
    lv_subject_t bed_mesh_probe_text_;          ///< "Probing point 5 of 25"
    lv_subject_t bed_mesh_probe_indeterminate_; ///< 1 = spinner (total unknown), 0 = progress bar
    lv_subject_t bed_mesh_error_message_;       ///< Error message if failed

    char probe_text_buf_[64];     ///< Buffer for probe_text_ subject
    char error_message_buf_[256]; ///< Buffer for error_message_ subject

    // ========== Modal Widget Pointers (uses ui_modal_show pattern) ==========
    lv_obj_t* calibrate_modal_widget_ = nullptr;
    lv_obj_t* rename_modal_widget_ = nullptr;
    lv_obj_t* save_config_modal_widget_ = nullptr;
    lv_obj_t* delete_modal_widget_ = nullptr;

    // ========== UI Widget Pointers ==========
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* profile_dropdown_ = nullptr;
    lv_obj_t* calibrate_name_input_ = nullptr;
    lv_obj_t* rename_name_input_ = nullptr;

    // ========== State ==========
    std::string pending_delete_profile_;
    std::string pending_rename_old_;
    std::string pending_rename_new_;
    enum class PendingOperation { None, Delete, Rename, Calibrate };
    PendingOperation pending_operation_ = PendingOperation::None;

    // Operation timeout guard (no subject needed — modals prevent interaction)
    OperationTimeoutGuard operation_guard_;
    static constexpr uint32_t OPERATION_TIMEOUT_MS = 15000; // quick ops (delete, rename)
    static constexpr uint32_t SLOW_OPERATION_TIMEOUT_MS =
        120000; // load, save_config (Klipper restart)
    static constexpr uint32_t CALIBRATION_TIMEOUT_MS = 300000; // 5 min for BED_MESH_CALIBRATE
    static constexpr double PROBE_NOZZLE_TEMP = 150.0; // °C — warm nozzle prevents ooze interference
    static constexpr double PROBE_BED_TEMP = 60.0;     // °C — thermal expansion for accurate mesh

    // RAII subscription guard - auto-unsubscribes from Moonraker on destruction
    SubscriptionGuard subscription_;

    // Observer for build_volume changes to refresh bed bounds
    ObserverGuard build_volume_observer_;

    // Cached mesh bounds for refreshing when build_volume changes
    double cached_mesh_min_x_ = 0.0;
    double cached_mesh_max_x_ = 0.0;
    double cached_mesh_min_y_ = 0.0;
    double cached_mesh_max_y_ = 0.0;
    bool has_cached_mesh_bounds_ = false;

    // Pending mesh data - stored until build_volume is available
    std::vector<std::vector<float>> pending_mesh_data_;
    bool has_pending_mesh_data_ = false;

    lv_obj_t* parent_screen_ = nullptr;
    bool callbacks_registered_ = false;

    // Preheat tracking — true when we turned on a heater that was off before probing
    bool preheat_turned_on_nozzle_ = false;
    bool preheat_turned_on_bed_ = false;

    // ========== Private Methods ==========
    void preheat_for_probing();
    void cooldown_after_probing();
    void start_home_and_probe();
    void setup_profile_dropdown();
    void setup_moonraker_subscription();
    void setup_build_volume_observer();
    void refresh_bed_bounds();
    void on_mesh_update_internal(const BedMeshProfile& mesh);
    void update_profile_list_subjects();
    void update_info_subjects(const std::vector<std::vector<float>>& mesh_data, int cols, int rows);
    void ensure_async_rendering();

    // Calculate range (variance) for a profile
    float calculate_profile_range(const std::string& profile_name);

    // Profile operation implementations
    void execute_delete_profile(const std::string& name);
    void execute_rename_profile(const std::string& old_name, const std::string& new_name);
    void execute_calibration(const std::string& profile_name);
    void execute_save_config();

    static void on_profile_dropdown_changed(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
BedMeshPanel& get_global_bed_mesh_panel();

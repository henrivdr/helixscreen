// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "ui_modal.h"

#include <functional>
#include <string>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Configuration modal for the camera panel widget
 *
 * Allows configuring:
 * - Rotation (0°, 90°, 180°, 270°)
 * - Flip horizontal
 * - Flip vertical
 *
 * Uses C++-owned subjects for reactive XML bindings (per-button selected state).
 * Opened from grid edit mode via the gear icon on the camera widget.
 */
class CameraConfigModal : public Modal {
  public:
    using SaveCallback = std::function<void(const nlohmann::json& config)>;

    CameraConfigModal(const std::string& widget_id, const std::string& panel_id,
                      SaveCallback on_save = nullptr);
    ~CameraConfigModal() override;

    const char* get_name() const override {
        return "Camera Config";
    }
    const char* component_name() const override {
        return "camera_config_modal";
    }

    // Static event callbacks — registered once in register_camera_widget()
    static void on_rotate_0(lv_event_t* e);
    static void on_rotate_90(lv_event_t* e);
    static void on_rotate_180(lv_event_t* e);
    static void on_rotate_270(lv_event_t* e);
    static void on_flip_h_toggled(lv_event_t* e);
    static void on_flip_v_toggled(lv_event_t* e);

  protected:
    void on_show() override;
    void on_ok() override;

  private:
    void init_subjects();
    void deinit_subjects();
    void sync_rotation_subjects();
    void sync_flip_subjects();

    std::string widget_id_;
    std::string panel_id_;
    SaveCallback on_save_;
    int rotation_ = 0; // 0, 90, 180, 270
    bool flip_h_ = false;
    bool flip_v_ = false;

    // Active instance for static event-callback routing. Only one camera config
    // modal is open at a time (owned by the single CameraWidget). Avoids walking
    // the parent chain for user_data, which can land on a ui_button's own
    // user_data and miscast (L069).
    static CameraConfigModal* s_active_;

    // C++-owned subjects for XML bindings
    bool subjects_initialized_ = false;
    lv_subject_t rot_0_active_;
    lv_subject_t rot_90_active_;
    lv_subject_t rot_180_active_;
    lv_subject_t rot_270_active_;
    lv_subject_t flip_h_active_;
    lv_subject_t flip_v_active_;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA

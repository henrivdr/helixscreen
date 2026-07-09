// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_config_modal.h"

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "panel_widget_config.h"
#include "panel_widget_manager.h"

#include <spdlog/spdlog.h>

namespace helix {

CameraConfigModal* CameraConfigModal::s_active_ = nullptr;

CameraConfigModal::CameraConfigModal(const std::string& widget_id, const std::string& panel_id,
                                     SaveCallback on_save)
    : widget_id_(widget_id), panel_id_(panel_id), on_save_(std::move(on_save)) {
    init_subjects();
    s_active_ = this;
}

CameraConfigModal::~CameraConfigModal() {
    if (s_active_ == this)
        s_active_ = nullptr;
    deinit_subjects();
}

void CameraConfigModal::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_int(&rot_0_active_, 1);
    lv_subject_init_int(&rot_90_active_, 0);
    lv_subject_init_int(&rot_180_active_, 0);
    lv_subject_init_int(&rot_270_active_, 0);
    lv_subject_init_int(&flip_h_active_, 0);
    lv_subject_init_int(&flip_v_active_, 0);

    lv_xml_register_subject(nullptr, "cam_rot_0_active", &rot_0_active_);
    lv_xml_register_subject(nullptr, "cam_rot_90_active", &rot_90_active_);
    lv_xml_register_subject(nullptr, "cam_rot_180_active", &rot_180_active_);
    lv_xml_register_subject(nullptr, "cam_rot_270_active", &rot_270_active_);
    lv_xml_register_subject(nullptr, "cam_flip_h_active", &flip_h_active_);
    lv_xml_register_subject(nullptr, "cam_flip_v_active", &flip_v_active_);

    subjects_initialized_ = true;
}

void CameraConfigModal::deinit_subjects() {
    if (!subjects_initialized_)
        return;
    lv_subject_deinit(&rot_0_active_);
    lv_subject_deinit(&rot_90_active_);
    lv_subject_deinit(&rot_180_active_);
    lv_subject_deinit(&rot_270_active_);
    lv_subject_deinit(&flip_h_active_);
    lv_subject_deinit(&flip_v_active_);
    subjects_initialized_ = false;
}

void CameraConfigModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    if (dialog())
        lv_obj_set_user_data(dialog(), this);

    // Load current config
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    auto config = wc.get_widget_config(widget_id_);

    if (config.contains("rotation") && config["rotation"].is_number_integer())
        rotation_ = config["rotation"].get<int>();
    if (config.contains("flip_h") && config["flip_h"].is_boolean())
        flip_h_ = config["flip_h"].get<bool>();
    if (config.contains("flip_v") && config["flip_v"].is_boolean())
        flip_v_ = config["flip_v"].get<bool>();

    sync_rotation_subjects();
    sync_flip_subjects();

    spdlog::debug("[CameraConfig] Opened: rotation={}, flip_h={}, flip_v={}", rotation_, flip_h_,
                  flip_v_);
}

void CameraConfigModal::on_ok() {
    nlohmann::json config;
    config["rotation"] = rotation_;
    config["flip_h"] = flip_h_;
    config["flip_v"] = flip_v_;

    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    wc.set_widget_config(widget_id_, config);

    if (on_save_)
        on_save_(config);

    spdlog::info("[CameraConfig] Saved: rotation={}, flip_h={}, flip_v={}", rotation_, flip_h_,
                 flip_v_);
    hide();
}

void CameraConfigModal::sync_rotation_subjects() {
    lv_subject_set_int(&rot_0_active_, rotation_ == 0 ? 1 : 0);
    lv_subject_set_int(&rot_90_active_, rotation_ == 90 ? 1 : 0);
    lv_subject_set_int(&rot_180_active_, rotation_ == 180 ? 1 : 0);
    lv_subject_set_int(&rot_270_active_, rotation_ == 270 ? 1 : 0);
}

void CameraConfigModal::sync_flip_subjects() {
    lv_subject_set_int(&flip_h_active_, flip_h_ ? 1 : 0);
    lv_subject_set_int(&flip_v_active_, flip_v_ ? 1 : 0);
}

// Static event callbacks — routed through the single active instance (s_active_).
void CameraConfigModal::on_rotate_0(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->rotation_ = 0;
        m->sync_rotation_subjects();
    }
}
void CameraConfigModal::on_rotate_90(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->rotation_ = 90;
        m->sync_rotation_subjects();
    }
}
void CameraConfigModal::on_rotate_180(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->rotation_ = 180;
        m->sync_rotation_subjects();
    }
}
void CameraConfigModal::on_rotate_270(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->rotation_ = 270;
        m->sync_rotation_subjects();
    }
}

void CameraConfigModal::on_flip_h_toggled(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->flip_h_ = !m->flip_h_;
        m->sync_flip_subjects();
    }
}

void CameraConfigModal::on_flip_v_toggled(lv_event_t* e) {
    (void)e;
    if (auto* m = s_active_) {
        m->flip_v_ = !m->flip_v_;
        m->sync_flip_subjects();
    }
}

} // namespace helix

#endif // HELIX_HAS_CAMERA

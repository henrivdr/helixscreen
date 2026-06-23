// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_security.cpp
 * @brief Security Settings overlay — PIN management and auto-lock configuration.
 */

#include "ui_settings_security.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_pin_entry_modal.h"
#include "ui_toast_manager.h"

#include "lock_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

using helix::ui::PinEntryModal;

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SecuritySettingsOverlay> g_security_settings_overlay;

SecuritySettingsOverlay& get_security_settings_overlay() {
    if (!g_security_settings_overlay) {
        g_security_settings_overlay = std::make_unique<SecuritySettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SecuritySettingsOverlay", []() { g_security_settings_overlay.reset(); });
    }
    return *g_security_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SecuritySettingsOverlay::SecuritySettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SecuritySettingsOverlay::~SecuritySettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SecuritySettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    // No additional subjects needed — lock_pin_set is a global subject
    // registered by LockManager::init_subjects(), which is called at startup.
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (using global lock_pin_set)", get_name());
}

void SecuritySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_set_pin_clicked", on_set_pin_clicked},
        {"on_change_pin_clicked", on_change_pin_clicked},
        {"on_remove_pin_clicked", on_remove_pin_clicked},
        {"on_auto_lock_changed", on_auto_lock_changed},
    });
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SecuritySettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "security_settings_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SecuritySettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show — overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void SecuritySettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    init_auto_lock_toggle();
}

void SecuritySettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void SecuritySettingsOverlay::init_auto_lock_toggle() {
    if (!overlay_root_) {
        return;
    }
    lv_obj_t* auto_lock_row = lv_obj_find_by_name(overlay_root_, "row_auto_lock");
    if (!auto_lock_row) {
        return;
    }
    lv_obj_t* toggle = lv_obj_find_by_name(auto_lock_row, "toggle");
    if (!toggle) {
        return;
    }
    if (helix::LockManager::instance().auto_lock_enabled()) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    }
    spdlog::trace("[{}] Auto-lock toggle initialized ({})", get_name(),
                  helix::LockManager::instance().auto_lock_enabled() ? "ON" : "OFF");
}

/**
 * Two-step Set PIN flow: "Enter New PIN" → "Confirm PIN".
 * On match, the PIN is saved. On mismatch, error toast is shown.
 */
void SecuritySettingsOverlay::run_set_pin_flow() {
    PinEntryModal::show_pin_entry("Enter New PIN", [](const std::string& pin1) -> std::string {
        if (pin1.empty()) {
            spdlog::debug("[SecuritySettings] Set PIN cancelled at step 1");
            return "";
        }
        // Second entry for confirmation
        PinEntryModal::show_pin_entry(
            "Confirm PIN", [pin1](const std::string& pin2) -> std::string {
                if (pin2.empty()) {
                    spdlog::debug("[SecuritySettings] Set PIN cancelled at step 2");
                    return "";
                }
                if (pin1 != pin2) {
                    spdlog::info("[SecuritySettings] PIN confirmation mismatch");
                    return "PINs don't match";
                }
                if (helix::LockManager::instance().set_pin(pin1)) {
                    spdlog::info("[SecuritySettings] PIN set successfully");
                    ToastManager::instance().show(ToastSeverity::SUCCESS, "PIN set");
                } else {
                    spdlog::warn("[SecuritySettings] set_pin() failed (invalid length?)");
                    ToastManager::instance().show(ToastSeverity::ERROR, "PIN must be 4-6 digits");
                }
                return "";
            });
        return "";
    });
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SecuritySettingsOverlay::handle_set_pin_clicked() {
    spdlog::info("[{}] Set PIN clicked", get_name());
    run_set_pin_flow();
}

void SecuritySettingsOverlay::handle_change_pin_clicked() {
    spdlog::info("[{}] Change PIN clicked", get_name());

    PinEntryModal::show_pin_entry(
        "Enter Current PIN", [](const std::string& current) -> std::string {
            if (current.empty()) {
                spdlog::debug("[SecuritySettings] Change PIN cancelled at current PIN step");
                return "";
            }
            if (!helix::LockManager::instance().verify_pin(current)) {
                spdlog::info("[SecuritySettings] Change PIN: wrong current PIN");
                return "Wrong PIN";
            }
            // Current PIN verified — proceed to set new PIN
            get_security_settings_overlay().run_set_pin_flow();
            return "";
        });
}

void SecuritySettingsOverlay::handle_remove_pin_clicked() {
    spdlog::info("[{}] Remove PIN clicked", get_name());

    PinEntryModal::show_pin_entry(
        "Enter Current PIN", [](const std::string& current) -> std::string {
            if (current.empty()) {
                spdlog::debug("[SecuritySettings] Remove PIN cancelled");
                return "";
            }
            if (!helix::LockManager::instance().verify_pin(current)) {
                spdlog::info("[SecuritySettings] Remove PIN: wrong PIN");
                return "Wrong PIN";
            }
            helix::LockManager::instance().remove_pin();
            spdlog::info("[SecuritySettings] PIN removed");
            ToastManager::instance().show(ToastSeverity::SUCCESS, "PIN removed");
            return "";
        });
}

void SecuritySettingsOverlay::handle_auto_lock_changed(bool enabled) {
    spdlog::info("[{}] Auto-lock toggled: {}", get_name(), enabled ? "ON" : "OFF");
    helix::LockManager::instance().set_auto_lock(enabled);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SecuritySettingsOverlay::on_set_pin_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SecuritySettingsOverlay] on_set_pin_clicked");
    get_security_settings_overlay().handle_set_pin_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SecuritySettingsOverlay::on_change_pin_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SecuritySettingsOverlay] on_change_pin_clicked");
    get_security_settings_overlay().handle_change_pin_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SecuritySettingsOverlay::on_remove_pin_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SecuritySettingsOverlay] on_remove_pin_clicked");
    get_security_settings_overlay().handle_remove_pin_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SecuritySettingsOverlay::on_auto_lock_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SecuritySettingsOverlay] on_auto_lock_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_security_settings_overlay().handle_auto_lock_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings

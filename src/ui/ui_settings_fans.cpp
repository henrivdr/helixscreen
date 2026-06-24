// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_fans.cpp
 * @brief Implementation of FanSettingsOverlay
 */

#include "ui_settings_fans.h"

#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_status_pill.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

// ============================================================================
// STATIC RENAME MODAL CALLBACKS
// ============================================================================

static void on_fan_rename_confirm(lv_event_t* /*e*/) {
    helix::settings::get_fan_settings_overlay().confirm_rename();
}

static void on_fan_rename_cancel(lv_event_t* /*e*/) {
    helix::settings::get_fan_settings_overlay().cancel_rename();
}

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<FanSettingsOverlay> g_fan_settings_overlay;

FanSettingsOverlay& get_fan_settings_overlay() {
    if (!g_fan_settings_overlay) {
        g_fan_settings_overlay = std::make_unique<FanSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy("FanSettingsOverlay",
                                                         []() { g_fan_settings_overlay.reset(); });
    }
    return *g_fan_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

FanSettingsOverlay::FanSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

FanSettingsOverlay::~FanSettingsOverlay() {
    if (rename_subject_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&fan_rename_old_name_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void FanSettingsOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_fan_rename_confirm", on_fan_rename_confirm);
    lv_xml_register_event_cb(nullptr, "on_fan_rename_cancel", on_fan_rename_cancel);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* FanSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "fan_settings_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find containers for dynamic row population
    controllable_list_ = lv_obj_find_by_name(overlay_root_, "controllable_fans_list");
    auto_list_ = lv_obj_find_by_name(overlay_root_, "auto_fans_list");
    no_fans_placeholder_ = lv_obj_find_by_name(overlay_root_, "no_fans_placeholder");

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void FanSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Populate fan lists (also called on_activate for re-entry)
    populate_fans();

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void FanSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_fans();
}

void FanSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    cancel_rename(); // Dismiss rename modal if open
}

// ============================================================================
// FAN TYPE HELPERS
// ============================================================================

namespace {

/// Convert FanType to a short display string for the type pill
const char* fan_type_label(helix::FanType type) {
    switch (type) {
    case helix::FanType::PART_COOLING:
        return lv_tr("Part");
    case helix::FanType::HEATER_FAN:
        return lv_tr("Heater");
    case helix::FanType::CONTROLLER_FAN:
        return lv_tr("Controller");
    case helix::FanType::TEMPERATURE_FAN:
        return lv_tr("Temp");
    case helix::FanType::GENERIC_FAN:
        return lv_tr("Generic");
    case helix::FanType::OUTPUT_PIN_FAN:
        return lv_tr("Output Pin");
    }
    return lv_tr("Fan");
}

} // namespace

// ============================================================================
// FAN LIST POPULATION
// ============================================================================

void FanSettingsOverlay::update_section_count(const char* badge_name, size_t count) {
    if (!overlay_root_)
        return;

    lv_obj_t* badge = lv_obj_find_by_name(overlay_root_, badge_name);
    if (badge) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", count);
        ui_status_pill_set_text(badge, buf);
    }
}

void FanSettingsOverlay::populate_fan_list(lv_obj_t* list, bool controllable) {
    if (!list)
        return;

    // Clear existing rows
    uint32_t child_count = lv_obj_get_child_count(list);
    for (int i = static_cast<int>(child_count) - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(list, i);
        helix::ui::safe_delete(child);
    }

    auto& fans = get_printer_state().get_fans();
    size_t count = 0;

    for (const auto& fan : fans) {
        if (fan.is_controllable != controllable) {
            continue;
        }

        // Build speed string
        char speed_buf[16];
        snprintf(speed_buf, sizeof(speed_buf), "%d%%", fan.speed_percent);

        // Create row from XML component
        const char* type_label = fan_type_label(fan.type);
        const char* attrs[] = {
            "fan_name",   fan.display_name.c_str(), "fan_type", type_label,
            "fan_object", fan.object_name.c_str(),  nullptr,
        };
        auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "fan_settings_row", attrs));
        if (!row) {
            spdlog::warn("[{}] Failed to create row for fan: {}", get_name(), fan.object_name);
            continue;
        }

        // Update the speed label (not bound to a subject here — refreshed on populate)
        lv_obj_t* speed_label = lv_obj_find_by_name(row, "speed_label");
        if (speed_label) {
            lv_label_set_text(speed_label, speed_buf);
        }

        // Wire click callback on the entire row for rename
        {
            auto* obj_name = new std::string(fan.object_name);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            // Touch feedback: dim on press
            lv_obj_set_style_opa(row, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_add_event_cb(
                row,
                [](lv_event_t* e) {
                    LVGL_SAFE_EVENT_CB_BEGIN("[FanSettingsOverlay] row clicked");
                    auto* name = static_cast<std::string*>(lv_event_get_user_data(e));
                    if (name) {
                        lv_obj_t* r = lv_event_get_current_target_obj(e);
                        lv_obj_t* label = lv_obj_find_by_name(r, "name_label");
                        const char* current = label ? lv_label_get_text(label) : "";
                        get_fan_settings_overlay().handle_fan_rename(*name, current ? current : "");
                    }
                    LVGL_SAFE_EVENT_CB_END();
                },
                LV_EVENT_CLICKED, obj_name);

            lv_obj_add_event_cb(
                row,
                [](lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); },
                LV_EVENT_DELETE, obj_name);
        }

        ++count;
    }

    spdlog::debug("[{}] Populated {} {} fans", get_name(), count,
                  controllable ? "controllable" : "auto");
}

void FanSettingsOverlay::populate_fans() {
    if (!overlay_root_)
        return;

    auto& fans = get_printer_state().get_fans();

    // Count fans by category
    size_t controllable_count = 0;
    size_t auto_count = 0;
    for (const auto& fan : fans) {
        if (fan.is_controllable) {
            ++controllable_count;
        } else {
            ++auto_count;
        }
    }

    // Update section badges
    update_section_count("controllable_fan_count", controllable_count);
    update_section_count("auto_fan_count", auto_count);

    // Show/hide sections based on fan counts
    lv_obj_t* controllable_section = lv_obj_find_by_name(overlay_root_, "controllable_section");
    lv_obj_t* auto_section = lv_obj_find_by_name(overlay_root_, "auto_section");

    if (controllable_section) {
        if (controllable_count > 0)
            lv_obj_remove_flag(controllable_section, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(controllable_section, LV_OBJ_FLAG_HIDDEN);
    }
    if (auto_section) {
        if (auto_count > 0)
            lv_obj_remove_flag(auto_section, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(auto_section, LV_OBJ_FLAG_HIDDEN);
    }

    // Show empty state if no fans at all
    if (no_fans_placeholder_) {
        if (fans.empty())
            lv_obj_remove_flag(no_fans_placeholder_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(no_fans_placeholder_, LV_OBJ_FLAG_HIDDEN);
    }

    // Populate row lists
    populate_fan_list(controllable_list_, true);
    populate_fan_list(auto_list_, false);
}

// ============================================================================
// FAN RENAME
// ============================================================================

void FanSettingsOverlay::handle_fan_rename(const std::string& object_name,
                                           const std::string& current_name) {
    spdlog::info("[{}] Rename requested: '{}' (current: '{}')", get_name(), object_name,
                 current_name);

    // Lazy-init the subject on first use (avoids startup init order issues)
    if (!rename_subject_initialized_) {
        std::memset(rename_old_name_buf_, 0, sizeof(rename_old_name_buf_));
        lv_subject_init_string(&fan_rename_old_name_, rename_old_name_buf_, nullptr,
                               sizeof(rename_old_name_buf_), "");
        lv_xml_register_subject(nullptr, "fan_rename_old_name", &fan_rename_old_name_);
        rename_subject_initialized_ = true;
    }

    pending_rename_object_ = object_name;
    lv_subject_copy_string(&fan_rename_old_name_, current_name.c_str());

    rename_modal_ = helix::ui::modal_show("fan_rename_modal");
    if (!rename_modal_) {
        spdlog::error("[{}] Failed to show fan_rename_modal", get_name());
        return;
    }

    // Pre-fill input with current name
    lv_obj_t* input = lv_obj_find_by_name(rename_modal_, "fan_rename_new_name_input");
    if (input) {
        lv_textarea_set_text(input, current_name.c_str());
    }
}

void FanSettingsOverlay::confirm_rename() {
    if (pending_rename_object_.empty()) {
        cancel_rename();
        return;
    }

    // Find the text input in the modal
    lv_obj_t* input = lv_obj_find_by_name(lv_layer_top(), "fan_rename_new_name_input");
    if (!input) {
        input = lv_obj_find_by_name(lv_screen_active(), "fan_rename_new_name_input");
    }

    std::string new_name;
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && std::strlen(text) > 0) {
            new_name = text;
        }
    }

    if (!new_name.empty()) {
        get_printer_state().rename_fan(pending_rename_object_, new_name);
        spdlog::info("[{}] Renamed '{}' -> '{}'", get_name(), pending_rename_object_, new_name);
    }

    pending_rename_object_.clear();

    if (rename_modal_) {
        helix::ui::modal_hide(rename_modal_);
        rename_modal_ = nullptr;
    }

    // Refresh settings list to show new name
    if (overlay_root_) {
        populate_fans();
    }
}

void FanSettingsOverlay::cancel_rename() {
    pending_rename_object_.clear();

    if (rename_modal_) {
        helix::ui::modal_hide(rename_modal_);
        rename_modal_ = nullptr;
    }
}

} // namespace helix::settings

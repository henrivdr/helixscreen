// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_sound_preview_overlay.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "sound_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_map>

namespace helix::settings {

// ==========================================================================
// Singleton
// ==========================================================================

static std::unique_ptr<SoundPreviewOverlay> g_sound_preview_overlay;

SoundPreviewOverlay& get_sound_preview_overlay() {
    if (!g_sound_preview_overlay) {
        g_sound_preview_overlay = std::make_unique<SoundPreviewOverlay>();
        StaticPanelRegistry::instance().register_destroy("SoundPreviewOverlay",
                                                         []() { g_sound_preview_overlay.reset(); });
    }
    return *g_sound_preview_overlay;
}

// ==========================================================================
// Lifecycle
// ==========================================================================

SoundPreviewOverlay::SoundPreviewOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

void SoundPreviewOverlay::init_subjects() {
    init_subjects_guarded(
        [this]() { spdlog::debug("[{}] Subjects initialized (none needed)", get_name()); });
}

void SoundPreviewOverlay::register_callbacks() {
    spdlog::debug("[{}] Callbacks registered (none needed)", get_name());
}

lv_obj_t* SoundPreviewOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "sound_preview_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SoundPreviewOverlay::show(lv_obj_t* parent_screen) {
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
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

void SoundPreviewOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_buttons();
}

void SoundPreviewOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
    clear_buttons();
}

// ==========================================================================
// Button grid
// ==========================================================================

std::string SoundPreviewOverlay::display_name(const std::string& sound_name) {
    static const std::unordered_map<std::string, std::string> names = {
        {"button_tap", "Button Tap"},
        {"toggle_on", "Toggle On"},
        {"toggle_off", "Toggle Off"},
        {"nav_forward", "Nav Forward"},
        {"nav_back", "Nav Back"},
        {"dropdown_open", "Dropdown"},
        {"print_complete", "Print Complete"},
        {"print_cancelled", "Print Cancelled"},
        {"error_alert", "Error Alert"},
        {"error_tone", "Error Tone"},
        {"alarm_urgent", "Alarm Urgent"},
        {"test_beep", "Test Beep"},
        {"startup", "Startup"},
    };

    auto it = names.find(sound_name);
    if (it != names.end())
        return it->second;

    // Auto-title-case unknown names from custom themes
    std::string result;
    bool capitalize = true;
    for (char c : sound_name) {
        if (c == '_') {
            result += ' ';
            capitalize = true;
        } else if (capitalize) {
            result += static_cast<char>(toupper(static_cast<unsigned char>(c)));
            capitalize = false;
        } else {
            result += c;
        }
    }
    return result;
}

void SoundPreviewOverlay::populate_buttons() {
    lv_obj_t* grid = lv_obj_find_by_name(overlay_root_, "sound_button_grid");
    if (!grid) {
        spdlog::error("[{}] Could not find sound_button_grid", get_name());
        return;
    }

    auto sound_names = helix::SoundManager::instance().get_sound_names();
    spdlog::debug("[{}] Populating {} sound buttons", get_name(), sound_names.size());

    for (const auto& name : sound_names) {
        std::string label = display_name(name);
        const char* attrs[] = {"variant", "outline", "text", label.c_str(), nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(grid, "ui_button", attrs));
        if (!btn) {
            spdlog::warn("[{}] Failed to create button for '{}'", get_name(), name);
            continue;
        }

        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        // Suppress default button_tap sound — preview plays its own sound
        lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_4);

        // Store sound name in a heap-allocated string, freed when button is deleted
        auto* sound_name_ptr = new std::string(name);
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[SoundPreviewOverlay] button_click");
                auto* sname = static_cast<std::string*>(lv_event_get_user_data(e));
                if (sname) {
                    spdlog::debug("[SoundPreviewOverlay] Playing '{}'", *sname);
                    helix::SoundManager::instance().play(*sname);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, sound_name_ptr);

        // Clean up the heap string when the button is deleted
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                auto* sname = static_cast<std::string*>(lv_event_get_user_data(e));
                delete sname;
            },
            LV_EVENT_DELETE, sound_name_ptr);
    }
}

void SoundPreviewOverlay::clear_buttons() {
    lv_obj_t* grid = lv_obj_find_by_name(overlay_root_, "sound_button_grid");
    if (grid) {
        helix::ui::safe_clean_children(grid);
    }
}

} // namespace helix::settings

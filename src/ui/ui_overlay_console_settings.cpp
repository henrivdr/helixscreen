// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_console_settings.h"

#include "ui_nav_manager.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace {
std::unique_ptr<ConsoleSettingsOverlay> g_console_settings;
lv_obj_t* g_console_settings_panel = nullptr;

void on_filter_temps_changed(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    helix::SettingsManager::instance().set_console_filter_temps(enabled);
}

void on_filter_firmware_noise_changed(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    helix::SettingsManager::instance().set_console_filter_firmware_noise(enabled);
}
} // namespace

ConsoleSettingsOverlay& get_global_console_settings() {
    if (!g_console_settings) {
        spdlog::error(
            "[Console Settings] get_global_console_settings() called before initialization!");
        throw std::runtime_error("ConsoleSettingsOverlay not initialized");
    }
    return *g_console_settings;
}

void init_global_console_settings() {
    if (g_console_settings) {
        spdlog::warn("[Console Settings] ConsoleSettingsOverlay already initialized, skipping");
        return;
    }
    g_console_settings = std::make_unique<ConsoleSettingsOverlay>();
    StaticPanelRegistry::instance().register_destroy("ConsoleSettingsOverlay", []() {
        if (g_console_settings_panel) {
            NavigationManager::instance().unregister_overlay_instance(g_console_settings_panel);
        }
        g_console_settings_panel = nullptr;
        g_console_settings.reset();
    });
    spdlog::trace("[Console Settings] ConsoleSettingsOverlay initialized");
}

ConsoleSettingsOverlay::ConsoleSettingsOverlay() {
    spdlog::debug("[{}] Constructor", get_name());
}

void ConsoleSettingsOverlay::init_subjects() {
    lv_xml_register_event_cb(nullptr, "on_console_filter_temps_changed", on_filter_temps_changed);
    lv_xml_register_event_cb(nullptr, "on_console_filter_firmware_noise_changed",
                             on_filter_firmware_noise_changed);
    spdlog::debug("[{}] init_subjects() — XML callbacks registered", get_name());
}

lv_obj_t* ConsoleSettingsOverlay::create(lv_obj_t* parent) {
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, get_xml_component_name(), nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    g_console_settings_panel = overlay_root_;
    return overlay_root_;
}

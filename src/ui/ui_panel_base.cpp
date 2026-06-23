// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_base.h"

#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

using namespace helix;

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PanelBase::PanelBase(PrinterState& printer_state, MoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {
    // Reserve reasonable capacity to avoid reallocations
    observers_.reserve(8);
}

PanelBase::~PanelBase() {
    cleanup_observers();
}

// ============================================================================
// MOVE SEMANTICS
// ============================================================================

PanelBase::PanelBase(PanelBase&& other) noexcept
    : printer_state_(other.printer_state_), api_(other.api_), panel_(other.panel_),
      parent_screen_(other.parent_screen_), subjects_initialized_(other.subjects_initialized_),
      observers_(std::move(other.observers_)) {
    // Clear source's state to prevent double-cleanup
    other.api_ = nullptr;
    other.panel_ = nullptr;
    other.parent_screen_ = nullptr;
    other.subjects_initialized_ = false;
    // other.observers_ is already moved (now empty)
}

PanelBase& PanelBase::operator=(PanelBase&& other) noexcept {
    if (this != &other) {
        // Clean up our observers first
        cleanup_observers();

        // Move state
        api_ = other.api_;
        panel_ = other.panel_;
        parent_screen_ = other.parent_screen_;
        subjects_initialized_ = other.subjects_initialized_;
        observers_ = std::move(other.observers_);

        // Clear source's state
        other.api_ = nullptr;
        other.panel_ = nullptr;
        other.parent_screen_ = nullptr;
        other.subjects_initialized_ = false;
    }
    return *this;
}

// ============================================================================
// CORE LIFECYCLE
// ============================================================================

void PanelBase::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    panel_ = panel;
    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        spdlog::warn("[{}] setup() called before init_subjects() - subjects may not bind",
                     get_name());
    }
}

// ============================================================================
// OBSERVER MANAGEMENT
// ============================================================================

void PanelBase::register_observer(lv_observer_t* observer) {
    if (observer) {
        observers_.push_back(observer);
    }
}

void PanelBase::cleanup_observers() {
    // Do NOT manually call lv_observer_remove() here. Manual removal frees the
    // observer, but LVGL's widget delete cascade will then fire the observer's
    // unsubscribe_on_delete_cb on freed memory → linked list corruption → crash.
    // Observers created with lv_subject_add_observer_obj() are auto-removed when
    // their associated widget is deleted. See ui_temp_display.cpp on_delete().
    observers_.clear();
}

void PanelBase::set_overlay_width() {
    if (!panel_ || !parent_screen_) {
        spdlog::warn("[{}] set_overlay_width() called before setup()", get_name());
        return;
    }

    ui_set_overlay_width(panel_, parent_screen_);
}

// ============================================================================
// HOT-RELOAD REBUILD
// ============================================================================

bool PanelBase::rebuild() {
    if (!panel_ || !parent_screen_) {
        spdlog::debug("[PanelBase::rebuild] {} — no widget yet, skipping", get_name());
        return false;
    }

    auto& nav = NavigationManager::instance();
    auto id = nav.find_panel_id(this);
    if (id == helix::PanelId::Count) {
        spdlog::debug("[PanelBase::rebuild] {} — not registered with NavigationManager, skipping",
                      get_name());
        return false;
    }

    lv_obj_t* parent = lv_obj_get_parent(panel_);
    if (!parent) {
        spdlog::warn("[PanelBase::rebuild] {} — widget has no parent, skipping", get_name());
        return false;
    }

    const char* component = get_xml_component_name();
    spdlog::info("[PanelBase::rebuild] {} ({}) — tearing down and re-creating", get_name(),
                 component);

    bool was_hidden = lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN);

    on_deactivate();
    cleanup_observers();

    lv_obj_t* old_widget = panel_;
    panel_ = nullptr;

    auto* new_widget = static_cast<lv_obj_t*>(lv_xml_create(parent, component, nullptr));
    if (!new_widget) {
        spdlog::error("[PanelBase::rebuild] {} — lv_xml_create failed, leaving old widget in place",
                      component);
        panel_ = old_widget;
        return false;
    }

    if (was_hidden) {
        lv_obj_add_flag(new_widget, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(new_widget, LV_OBJ_FLAG_HIDDEN);
    }

    setup(new_widget, parent_screen_);

    nav.replace_panel_widget(id, new_widget);

    helix::ui::safe_delete_deferred(old_widget);

    if (!was_hidden) {
        on_activate();
    }
    return true;
}

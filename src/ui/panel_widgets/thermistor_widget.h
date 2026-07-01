// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

/// Home widget displaying a user-selected temperature sensor reading.
/// Click opens a context menu to choose which sensor to monitor.
/// Selection persists via PanelWidgetConfig per-widget config.
class ThermistorWidget : public PanelWidget {
  public:
    explicit ThermistorWidget(const std::string& instance_id);
    ~ThermistorWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override;
    const char* id() const override {
        return instance_id_.c_str();
    }
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Called from static event callback
    void handle_clicked();

    /// Select a sensor by klipper_name, update display, save config
    void select_sensor(const std::string& klipper_name);

    /// Select icon for this widget instance
    void select_icon(const std::string& name);

    // Static event callbacks (XML-registered)
    static void thermistor_clicked_cb(lv_event_t* e);
    static void thermistor_picker_backdrop_cb(lv_event_t* e);
    static void thermistor_picker_done_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    std::string icon_name_; // Custom icon, empty = "thermometer" default

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    nlohmann::json config_;
    std::vector<std::string> sensors_; // klipper_names for carousel mode

    std::string selected_sensor_; // klipper_name (e.g., "temperature_sensor mcu_temp")
    std::string display_name_;    // Pretty name for label
    ObserverGuard temp_observer_;
    SubjectLifetime temp_lifetime_;
    char temp_buffer_[16] = {};

    // Carousel mode
    struct CarouselPage {
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* name_label = nullptr;
        char temp_buffer[16] = {};
    };
    std::vector<CarouselPage> carousel_pages_;
    std::vector<ObserverGuard> carousel_observers_;
    std::vector<SubjectLifetime> carousel_lifetimes_;
    ObserverGuard version_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer (temp_observer_, carousel_observers_, version_observer_)
    // destructs. Without this, queued observer callbacks captured via
    // tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    bool binding_in_progress_ = false; // reentrancy guard for bind_carousel_sensors

    // Sensor picker context menu
    lv_obj_t* picker_backdrop_ = nullptr;

    bool is_carousel_mode() const;
    void attach_single();
    void attach_carousel();
    void bind_carousel_sensors();
    void show_configure_picker();
    void apply_sensor_selection(const std::vector<std::string>& selected);
    void resolve_display_name();
    void on_temp_changed(int decidegrees);
    void update_display();
    void save_config();
    void show_sensor_picker();
    void dismiss_sensor_picker();

    // Static active instance for picker event routing
    static ThermistorWidget* s_active_picker_;
};

} // namespace helix

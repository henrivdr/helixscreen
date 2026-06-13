// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heating_animator.h"
#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>

class TemperatureService;

namespace helix {
class PrinterState;

class TempStackWidget : public PanelWidget {
  public:
    TempStackWidget(PrinterState& printer_state, TemperatureService* temp_panel);
    ~TempStackWidget() override;

    void set_config(const nlohmann::json& config) override;
    std::string get_component_name() const override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    const char* id() const override {
        return "temp_stack";
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

  private:
    PrinterState& printer_state_;
    TemperatureService* temp_control_panel_;
    nlohmann::json config_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Heating icon animators
    HeatingIconAnimator nozzle_animator_;
    HeatingIconAnimator bed_animator_;

    // Cached temps (decidegrees)
    int cached_nozzle_temp_ = 25;
    int cached_nozzle_target_ = 0;
    int cached_bed_temp_ = 25;
    int cached_bed_target_ = 0;

    bool long_pressed_ = false;

    // Observers. The explicit detach() in ~TempStackWidget invalidates
    // lifetime_ before resetting these — that is the primary defense. The
    // member ordering below (lifetime_ declared LAST so it destructs FIRST)
    // is a safety net for future refactors: it guarantees that even if a
    // path destroys this widget without going through detach(), pending
    // queued observer callbacks see token.expired() == true and short-circuit
    // before touching the half-destroyed `self`. Bundle AX3CKAKB (k1 v0.99.52).
    ObserverGuard nozzle_temp_observer_;
    ObserverGuard nozzle_target_observer_;
    SubjectLifetime bed_temp_lifetime_;
    SubjectLifetime bed_target_lifetime_;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. See comment above.
    helix::AsyncLifetimeGuard lifetime_;

    bool is_carousel_mode() const;
    void attach_stack(lv_obj_t* widget_obj);
    void attach_carousel(lv_obj_t* widget_obj);

    void on_nozzle_temp_changed(int temp_deci);
    void on_nozzle_target_changed(int target_deci);
    void on_bed_temp_changed(int temp_deci);
    void on_bed_target_changed(int target_deci);

    void handle_nozzle_clicked();
    void handle_bed_clicked();
    void handle_chamber_clicked();

  public:
    // Public for early XML callback registration (before attach)
    static void temp_stack_nozzle_cb(lv_event_t* e);
    static void temp_stack_bed_cb(lv_event_t* e);
    static void temp_stack_chamber_cb(lv_event_t* e);

    // Carousel page click callback
    static void temp_carousel_page_cb(lv_event_t* e);
};

} // namespace helix

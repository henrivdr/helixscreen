// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "panel_widget.h"

class MoonrakerAPI;

namespace helix {

class PrinterState;

class LedControlsWidget : public PanelWidget {
  public:
    LedControlsWidget(PrinterState& printer_state, MoonrakerAPI* api);
    ~LedControlsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "led_controls";
    }

    static void on_led_controls_clicked(lv_event_t* e);

  private:
    void handle_clicked();

    PrinterState& printer_state_;
    MoonrakerAPI* api_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* led_control_panel_ = nullptr;
};

} // namespace helix

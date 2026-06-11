// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

namespace helix {

/// 2x1 home-panel widget with two print-control buttons: a primary
/// Pause/Resume button and a Stop button. Icon, label, enabled state, and
/// click handling are all owned by the shared helix::ui::PrintControlButtons
/// singleton — this widget is pure declarative binding, so it needs no
/// per-instance subjects or click routing.
class ControlButtonsWidget : public PanelWidget {
  public:
    ControlButtonsWidget() = default;
    ~ControlButtonsWidget() override = default;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "control_buttons";
    }

  private:
    lv_obj_t* widget_obj_ = nullptr;
};

} // namespace helix

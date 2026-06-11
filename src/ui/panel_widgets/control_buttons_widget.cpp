// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control_buttons_widget.h"

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_control_buttons_widget() {
    register_widget_factory("control_buttons", [](const std::string&) {
        return std::make_unique<ControlButtonsWidget>();
    });
    // No init_subjects: the print_control_* subjects are owned and registered
    // by the helix::ui::PrintControlButtons singleton at startup.
}

void ControlButtonsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    spdlog::debug("[ControlButtonsWidget] Attached");
}

void ControlButtonsWidget::detach() {
    widget_obj_ = nullptr;
    spdlog::debug("[ControlButtonsWidget] Detached");
}

} // namespace helix

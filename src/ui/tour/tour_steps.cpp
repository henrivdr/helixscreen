// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_steps.h"

#include "ams_state.h"

namespace helix::tour {

bool hardware_has_ams() {
    return AmsState::instance().backend_count() > 0;
}

std::vector<TourStep> build_tour_steps(bool has_ams) {
    std::vector<TourStep> steps;
    steps.reserve(8);

    // 1. Welcome (centered card, no target)
    steps.push_back(
        {"", "tour.step.welcome.title", "tour.step.welcome.body", TooltipAnchor::Center, {}});

    // 2. Home widget example — highlight a concrete tile. Widget root objects
    //    are named by their factory widget_id (see panel_widget_manager.cpp).
    //    Prefer AMS if the printer has one (most visually distinctive).
    steps.push_back({has_ams ? "ams" : "nozzle_temps",
                     "tour.step.home_grid.title",
                     "tour.step.home_grid.body",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 3. Long-press to customize — highlight a different tile so the user sees
    //    the edit-mode message paired with a concrete example they can try.
    steps.push_back({"fan_stack",
                     "tour.step.customize.title",
                     "tour.step.customize.body",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 4. Print status widget on home (navbar Print button lacks a highlightable
    //    name, and the print-status tile on home is more informative anyway).
    steps.push_back({"print_status",
                     "tour.step.print.title",
                     "tour.step.print.body",
                     TooltipAnchor::PreferBelow,
                     {}});

    // 5-8. Navbar tour
    steps.push_back({"nav_btn_controls",
                     "tour.step.controls.title",
                     "tour.step.controls.body",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_filament",
                     "tour.step.filament.title",
                     "tour.step.filament.body",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_advanced",
                     "tour.step.advanced.title",
                     "tour.step.advanced.body",
                     TooltipAnchor::PreferRight,
                     {}});
    steps.push_back({"nav_btn_settings",
                     "tour.step.settings.title",
                     "tour.step.settings.body",
                     TooltipAnchor::PreferRight,
                     {}});

    return steps;
}

} // namespace helix::tour

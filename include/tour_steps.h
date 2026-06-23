// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace helix::tour {

enum class TooltipAnchor {
    Center, ///< No target — tooltip screen-centered
    PreferBelow,
    PreferAbove,
    PreferRight, ///< Typical for navbar buttons (tooltip to the right of left-side navbar)
    PreferLeft,
};

struct TourStep {
    /// Target widget name for `lv_obj_find_by_name()`. Empty = centered, no highlight.
    std::string target_name;

    /// Translation keys for title and body (passed through `lv_tr()` at render time).
    std::string title_key;
    std::string body_key;

    TooltipAnchor anchor_hint = TooltipAnchor::PreferBelow;

    /// Additional widget names to sequentially spotlight inside this step (step 2 only).
    /// Each is highlighted for ~1.5s before the next. Empty = single target only.
    std::vector<std::string> sub_spotlights;
};

/// Build the tour step list. AMS sub-spotlight on step 2 only added when has_ams=true.
std::vector<TourStep> build_tour_steps(bool has_ams);

/// Convenience: queries `AmsState::instance().backend_count() > 0`.
bool hardware_has_ams();

} // namespace helix::tour

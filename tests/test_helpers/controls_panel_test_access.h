// tests/test_helpers/controls_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // ObserverGuard, SubjectLifetime
#include "ui_panel_controls.h"

#include <string>
#include <vector>

namespace helix::ui {

// Test-only access to ControlsPanel's private secondary-fan internals.
//
// Locks in the SubjectLifetime member-vector doctrine (L084/L077): the per-fan
// speed subjects are dynamic (freed + recreated on fan rediscovery), so each
// observer's lifetime token must live in a member vector that outlives the
// paired ObserverGuard — never a stack local. These accessors let the
// conformance test inspect that the member vectors exist, stay aligned, and
// that the retained token copies observe subject death on rediscovery.
struct ControlsPanelTestAccess {
    using SecondaryFanRow = ControlsPanel::SecondaryFanRow;

    static std::vector<SecondaryFanRow>& rows(ControlsPanel& p) {
        return p.secondary_fan_rows_;
    }

    static std::vector<ObserverGuard>& observers(ControlsPanel& p) {
        return p.secondary_fan_observers_;
    }

    static std::vector<SubjectLifetime>& lifetimes(ControlsPanel& p) {
        return p.secondary_fan_lifetimes_;
    }

    /// Seed the tracked rows directly from a list of Klipper object names.
    /// subscribe_to_secondary_fan_speeds() only reads row.object_name, so the
    /// speed_label may stay null.
    static void set_rows(ControlsPanel& p, const std::vector<std::string>& object_names) {
        p.secondary_fan_rows_.clear();
        for (const auto& name : object_names) {
            p.secondary_fan_rows_.push_back({name, nullptr});
        }
    }

    /// Invoke the private subscribe path under test.
    static void subscribe(ControlsPanel& p) {
        p.subscribe_to_secondary_fan_speeds();
    }

    /// Wire the container the real teardown/rebuild path (populate_secondary_fans)
    /// requires, so the production cleanup can be exercised end-to-end.
    static void set_fans_list(ControlsPanel& p, lv_obj_t* list) {
        p.secondary_fans_list_ = list;
    }

    /// Invoke the real production teardown + rebuild path.
    static void populate(ControlsPanel& p) {
        p.populate_secondary_fans();
    }
};

} // namespace helix::ui

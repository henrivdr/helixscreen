// tests/test_helpers/print_control_buttons_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_control_buttons.h"

namespace helix::ui {

// Test-only access to PrintControlButtons internals. The singleton persists
// across test cases, so reset() must fully tear it down for isolation.
struct PrintControlButtonsTestAccess {
    static void reset() {
        auto& s = PrintControlButtons::instance();
        s.print_state_observer_.reset();
        if (s.pending_action_timeout_) {
            lv_timer_delete(s.pending_action_timeout_);
            s.pending_action_timeout_ = nullptr;
        }
        s.subjects_.deinit_all();
        s.pending_action_ = PendingAction::None;
        s.subjects_initialized_ = false;
    }

    static void set_pending(PendingAction a) {
        PrintControlButtons::instance().start_pending_action(a);
    }

    static PendingAction pending() {
        return PrintControlButtons::instance().pending_action_;
    }
};

} // namespace helix::ui

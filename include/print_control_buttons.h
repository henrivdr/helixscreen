// include/print_control_buttons.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_control_view.h"
#include "subject_managed_panel.h"
#include "ui_observer_guard.h"
#include "ui_print_cancel_modal.h"
#include <lvgl.h>

class MoonrakerAPI;

namespace helix::ui {

/// Singleton owning the LVGL subjects for the two print-control buttons
/// (primary Pause/Resume + Stop). Observes the GLOBAL `print_state_enum`
/// subject independently and drives the subjects through the pure
/// compute_control_button_view() function. Multiple views (PrintStatusPanel,
/// future home widget) bind these subjects.
///
/// Click handlers and the cancel modal are stubs in this revision; they are
/// wired up in a later task.
class PrintControlButtons {
  public:
    static PrintControlButtons& instance();

    /// Initialize the owned subjects, register them into the global XML scope,
    /// wire the event callbacks, and start observing the print state. Idempotent.
    void init_subjects();

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    lv_subject_t* pending_action_subject() {
        return &pending_action_subject_;
    }

    void handle_primary_button();
    void handle_stop_button();

  private:
    PrintControlButtons() = default;

    void recompute();
    void start_pending_action(PendingAction action);
    void clear_pending_action();

    static void on_primary_clicked(lv_event_t* e);
    static void on_stop_clicked(lv_event_t* e);

    MoonrakerAPI* api_ = nullptr;
    bool subjects_initialized_ = false;
    PendingAction pending_action_ = PendingAction::None;
    lv_timer_t* pending_action_timeout_ = nullptr;

    SubjectManager subjects_;
    lv_subject_t primary_icon_subject_;
    lv_subject_t primary_label_subject_;
    lv_subject_t primary_enabled_subject_;
    lv_subject_t stop_enabled_subject_;
    lv_subject_t pending_action_subject_;
    char primary_icon_buf_[32] = "\xF3\xB0\x8F\xA4";
    char primary_label_buf_[16] = "Pause";

    // print_state_enum is a STATIC global subject (singleton lifetime), so a
    // bare member ObserverGuard with NO SubjectLifetime token is correct.
    // SubjectLifetime is only required for dynamic per-fan/sensor/extruder
    // subjects that can be destroyed and recreated during rediscovery.
    ObserverGuard print_state_observer_;
    PrintCancelModal cancel_modal_;

    friend struct PrintControlButtonsTestAccess;
};

} // namespace helix::ui

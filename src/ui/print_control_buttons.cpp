// src/ui/print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_buttons.h"

#include "app_globals.h" // get_printer_state()
#include "observer_factory.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix::ui {

PrintControlButtons& PrintControlButtons::instance() {
    static PrintControlButtons s_instance;
    return s_instance;
}

void PrintControlButtons::init_subjects() {
    if (subjects_initialized_)
        return;

    UI_MANAGED_SUBJECT_STRING(primary_icon_subject_, primary_icon_buf_, "\xF3\xB0\x8F\xA4",
                              "print_control_primary_icon", subjects_);
    UI_MANAGED_SUBJECT_STRING(primary_label_subject_, primary_label_buf_, "Pause",
                              "print_control_primary_label", subjects_);
    UI_MANAGED_SUBJECT_INT(primary_enabled_subject_, 0, "print_control_primary_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(stop_enabled_subject_, 0, "print_control_stop_enabled", subjects_);
    UI_MANAGED_SUBJECT_INT(pending_action_subject_, 0, "print_pending_action", subjects_);

    lv_xml_register_event_cb(nullptr, "on_print_control_primary", on_primary_clicked);
    lv_xml_register_event_cb(nullptr, "on_print_control_stop", on_stop_clicked);

    // print_state_enum is a static global subject — no SubjectLifetime needed.
    print_state_observer_ = observe_int_sync<PrintControlButtons>(
        get_printer_state().get_print_state_enum_subject(), this,
        [](PrintControlButtons* self, int) {
            // A real state change clears any optimistic pending action; the new
            // state is authoritative. Otherwise just recompute the buttons.
            if (self->pending_action_ != PendingAction::None)
                self->clear_pending_action();
            else
                self->recompute();
        });

    // Self-register cleanup so subjects/observer are torn down before lv_deinit().
    StaticSubjectRegistry::instance().register_deinit("PrintControlButtons", []() {
        auto& self = PrintControlButtons::instance();
        // release() (NOT reset()) is correct here: this runs pre-lv_deinit when
        // the observed subject is already being destroyed by its own owner.
        self.print_state_observer_.release();
        if (self.pending_action_timeout_) {
            lv_timer_delete(self.pending_action_timeout_);
            self.pending_action_timeout_ = nullptr;
        }
        self.subjects_.deinit_all();
        self.subjects_initialized_ = false;
    });

    subjects_initialized_ = true;
    recompute();
}

void PrintControlButtons::recompute() {
    if (!subjects_initialized_)
        return;

    auto state = static_cast<helix::PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));

    auto& macros = StandardMacros::instance();
    ControlButtonView v = compute_control_button_view(
        state, pending_action_, !macros.get(StandardMacroSlot::Pause).is_empty(),
        !macros.get(StandardMacroSlot::Resume).is_empty(),
        !macros.get(StandardMacroSlot::Cancel).is_empty());

    std::snprintf(primary_icon_buf_, sizeof(primary_icon_buf_), "%s", v.primary_icon);
    std::snprintf(primary_label_buf_, sizeof(primary_label_buf_), "%s", v.primary_label);
    lv_subject_copy_string(&primary_icon_subject_, primary_icon_buf_);
    lv_subject_copy_string(&primary_label_subject_, primary_label_buf_);
    lv_subject_set_int(&primary_enabled_subject_, v.primary_enabled ? 1 : 0);
    lv_subject_set_int(&stop_enabled_subject_, v.stop_enabled ? 1 : 0);
    lv_subject_set_int(&pending_action_subject_, static_cast<int>(pending_action_));
}

// Click handlers: STUBS until the next task wires up Pause/Resume RPCs and the
// cancel-confirmation modal.
void PrintControlButtons::handle_primary_button() {}
void PrintControlButtons::handle_stop_button() {}

void PrintControlButtons::start_pending_action(PendingAction action) {
    pending_action_ = action;
    recompute();
}

void PrintControlButtons::clear_pending_action() {
    pending_action_ = PendingAction::None;
    recompute();
}

void PrintControlButtons::on_primary_clicked(lv_event_t*) {
    instance().handle_primary_button();
}

void PrintControlButtons::on_stop_clicked(lv_event_t*) {
    instance().handle_stop_button();
}

} // namespace helix::ui

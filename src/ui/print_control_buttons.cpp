// src/ui/print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_buttons.h"

#include "abort_manager.h"
#include "app_globals.h" // get_printer_state()
#include "moonraker_api.h"
#include "moonraker_error.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "static_subject_registry.h"
#include "ui_error_reporting.h" // NOTIFY_WARNING / NOTIFY_ERROR
#include "ui_event_safety.h"    // LVGL_SAFE_EVENT_CB_BEGIN / END
#include "ui_resume_dispatch.h"

#include "lvgl/src/others/translation/lv_translation.h" // lv_tr

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

// PrintControlButtons is an app-lifetime singleton, so the stateless callbacks
// below reach back via PrintControlButtons::instance() rather than capturing
// `this` — the established safe pattern (mirrors PrintStatusPanel's old
// get_global_print_status_panel() usage). No use-after-free is possible.
void PrintControlButtons::handle_primary_button() {
    if (!api_) {
        spdlog::warn("[PrintControl] No API - cannot dispatch primary action");
        return;
    }
    auto state = static_cast<helix::PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    auto& macros = StandardMacros::instance();

    if (state == helix::PrintJobState::PRINTING) {
        if (macros.get(StandardMacroSlot::Pause).is_empty()) {
            NOTIFY_WARNING(lv_tr("Pause macro not configured"));
            return;
        }
        spdlog::info("[PrintControl] Pausing print");
        start_pending_action(PendingAction::Pausing);
        // suppress_auto_toast=true: the on_error below surfaces a contextual
        // toast; the generic RPC_ERROR auto-toast + Klipper's `!!` broadcast
        // for the same root cause would be redundant noise.
        macros.execute(
            StandardMacroSlot::Pause, api_,
            []() { spdlog::info("[PrintControl] Pause sent"); },
            [](const MoonrakerError& err) {
                spdlog::error("[PrintControl] Pause failed: {}", err.message);
                NOTIFY_ERROR(lv_tr("Failed to pause print: {}"), err.user_message());
                PrintControlButtons::instance().clear_pending_action();
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
    } else if (state == helix::PrintJobState::PAUSED) {
        if (macros.get(StandardMacroSlot::Resume).is_empty()) {
            NOTIFY_WARNING(lv_tr("Resume macro not configured"));
            return;
        }
        spdlog::info("[PrintControl] Resuming print");
        start_pending_action(PendingAction::Resuming);
        // dispatch_prepared_resume runs the backend prepare_for_resume → Resume
        // chain. The optimistic spinner spans the whole window; only clear on
        // failure (success waits on the PrinterState observer confirmation).
        helix::ui::dispatch_prepared_resume(
            api_, "[PrintControl]",
            []() { PrintControlButtons::instance().clear_pending_action(); });
    }
}

void PrintControlButtons::handle_stop_button() {
    spdlog::info("[PrintControl] Stop clicked - confirming");
    if (helix::AbortManager::instance().is_aborting()) {
        NOTIFY_WARNING(lv_tr("Abort already in progress"));
        return;
    }
    cancel_modal_.set_on_confirm([]() {
        spdlog::info("[PrintControl] Stop confirmed - starting AbortManager");
        // AbortManager handles its own UI state (progress modal, button states).
        helix::AbortManager::instance().start_abort();
    });
    cancel_modal_.show(lv_screen_active());
}

void PrintControlButtons::start_pending_action(PendingAction action) {
    clear_pending_action(); // supersede any in-flight action (also deletes prior timer)
    pending_action_ = action;
    // 25s safety net: if the real state never arrives (RPC ack lost, backend
    // wedged), clear the optimistic spinner so the button is usable again.
    pending_action_timeout_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<PrintControlButtons*>(lv_timer_get_user_data(t));
            if (!self)
                return;
            const char* verb =
                (self->pending_action_ == PendingAction::Resuming) ? "Resume" : "Pause";
            spdlog::warn("[PrintControl] {} timed out - clearing pending", verb);
            NOTIFY_WARNING(lv_tr("{} command timed out"), verb);
            self->clear_pending_action();
        },
        25000, this);
    lv_timer_set_repeat_count(pending_action_timeout_, 1);
    recompute();
}

void PrintControlButtons::clear_pending_action() {
    if (pending_action_timeout_) {
        lv_timer_delete(pending_action_timeout_);
        pending_action_timeout_ = nullptr;
    }
    pending_action_ = PendingAction::None;
    recompute();
}

void PrintControlButtons::on_primary_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintControl] on_primary_clicked");
    (void)e;
    instance().handle_primary_button();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintControlButtons::on_stop_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintControl] on_stop_clicked");
    (void)e;
    instance().handle_stop_button();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui

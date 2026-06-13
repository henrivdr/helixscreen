// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_runout_handler.h"

#include "ui_error_reporting.h"
#include "ui_nav_manager.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "filament_sensor_manager.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h" // For PrintState enum
#include "runtime_config.h"
#include "standard_macros.h"
#include "ui_resume_dispatch.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

// ============================================================================
// FilamentRunoutHandler Implementation
// ============================================================================

FilamentRunoutHandler::FilamentRunoutHandler(MoonrakerAPI* api) : api_(api) {
    spdlog::debug("[FilamentRunoutHandler] Constructed");
}

FilamentRunoutHandler::~FilamentRunoutHandler() {
    // lifetime_ destructor calls invalidate() automatically
    spdlog::trace("[FilamentRunoutHandler] Destroyed");
}

// ============================================================================
// State Transition Handler
// ============================================================================

void FilamentRunoutHandler::on_print_state_changed(::PrintState old_state, ::PrintState new_state) {
    (void)old_state;

    // Check for runout condition when entering Paused state
    if (new_state == ::PrintState::Paused) {
        check_and_show_runout_guidance();
    }

    // Reset runout modal flag and hide modal on print resume or end
    if (new_state == ::PrintState::Printing || new_state == ::PrintState::Idle ||
        new_state == ::PrintState::Complete || new_state == ::PrintState::Cancelled ||
        new_state == ::PrintState::Error) {
        runout_modal_shown_for_pause_ = false;
        user_took_manual_action_ = false;
        hide_runout_guidance_modal();
    }
}

// ============================================================================
// Runout Detection and Modal Display
// ============================================================================

void FilamentRunoutHandler::check_and_show_runout_guidance() {
    // Only show once per pause event
    if (runout_modal_shown_for_pause_) {
        return;
    }

    // Skip if AMS/MMU present and not forced (runout during swaps is normal)
    if (!get_runtime_config()->should_show_runout_modal()) {
        return;
    }

    auto& sensor_mgr = helix::FilamentSensorManager::instance();

    // Check if any runout sensor shows no filament
    if (sensor_mgr.has_any_runout()) {
        // Auto-recover-on-pause was previously gated on `motion=false AND
        // port=true` but field testing exposed two failure modes: (a) the
        // "port=true" signal alone doesn't prove filament reached the
        // extruder gear (e.g., Snapmaker assist motor pre-loads to ~4
        // inches short of the toolhead and stops); (b) firmware load
        // macros (AUTO_FEEDING/MANUAL_FEEDING) silently no-op outside an
        // active print, so the recovery chain can't actually move filament
        // either. Net result: silent air-prints. Pulled until we have a
        // verified detection signal AND a recovery path that observably
        // moves filament. Modal-driven Resume (user-initiated) still uses
        // backend->prepare_for_resume.
        spdlog::info(
            "[FilamentRunoutHandler] Runout detected during pause - showing guidance modal");
        show_runout_guidance_modal();
        runout_modal_shown_for_pause_ = true;
    }
}

void FilamentRunoutHandler::show_runout_guidance_modal() {
    if (runout_modal_.is_visible()) {
        // Already showing
        return;
    }

    // Fresh modal: re-arm the sensor-driven auto-close. Any in-dialog
    // Load/Unload/Purge will set this true to suppress auto-close.
    user_took_manual_action_ = false;
    // Re-arm the auto-close latch (#991): the observer must observe a confirmed
    // runout (value==1) on THIS modal before a clear (value==0) can auto-close,
    // so the observer's initial read / startup-grace transient cannot close it.
    runout_confirmed_active_ = false;

    spdlog::info("[FilamentRunoutHandler] Showing runout guidance modal");

    // Capability-aware layout: backends that feed filament to the nozzle as part
    // of resume (e.g. Snapmaker U1's AUTO_FEEDING) present Resume as the primary
    // action and demote manual Load/Unload/Purge. Set on the main thread (show
    // happens on the main thread), so a direct subject set is safe here.
    {
        AmsBackend* backend = AmsState::instance().get_backend();
        bool autofeed = backend && backend->recovers_filament_on_resume();
        runout_modal_.set_autofeed_capable(autofeed);
        spdlog::debug("[FilamentRunoutHandler] Runout dialog autofeed_capable={}", autofeed);
    }

    // Capture token for async callback safety
    auto token = lifetime_.token();

    // Configure callbacks for the six options
    runout_modal_.set_on_load_filament([this, token]() {
        // #991 diagnostic: the Load press was observed to close the modal but
        // perform no action. Log entry + token state BEFORE the guard so the
        // next on-device repro disambiguates "callback never ran" vs
        // "token expired and swallowed the action".
        spdlog::info("[FilamentRunoutHandler] Load callback entered (token expired={})",
                     token.expired());
        if (token.expired()) return;
        user_took_manual_action_ = true; // keep dialog open; suppress auto-close
        AmsBackend* backend = AmsState::instance().get_backend();
        if (backend) {
            int slot = backend->get_current_slot();
            spdlog::info("[FilamentRunoutHandler] User chose to load filament after runout (tool {})",
                         slot);
            AmsError err = backend->load_filament(slot);
            if (!err.success()) {
                spdlog::error("[FilamentRunoutHandler] Load filament failed: {}", err.technical_msg);
                NOTIFY_ERROR(lv_tr("Failed to load filament: {}"), err.user_msg);
            }
        } else {
            // No AMS backend — fall back to navigating to the Filament panel.
            spdlog::info("[FilamentRunoutHandler] No AMS backend; navigating to Filament panel to load");
            NavigationManager::instance().set_active(PanelId::Filament);
        }
    });

    runout_modal_.set_on_resume([this, token]() {
        if (token.expired()) return;

        // No client-side filament-present gate here. The previous
        // has_any_runout() check used the encoder-based motion sensor, which
        // only flips back to "present" after actual extrusion happens — so on
        // a tool-changer like the Snapmaker U1, where the user reloads a spool
        // at the buffer/port and the buffer auto-feeds to within a few inches
        // of the toolhead, the sensor stays in runout state until Klipper's
        // RESUME chain extrudes. Gating on the sensor refused legitimate user
        // intent ("Insert filament before resuming" while a fresh spool was
        // sitting in the buffer). Trust Klipper to enforce — INNER_RESUME has
        // its own CHECK_FILAMENT_RUNOUT, and any rejection now surfaces as a
        // single contextual error toast via the suppress_auto_toast path.

        // Check if resume slot is available
        const auto& resume_info = StandardMacros::instance().get(StandardMacroSlot::Resume);
        if (resume_info.is_empty()) {
            spdlog::warn("[FilamentRunoutHandler] Resume macro slot is empty");
            NOTIFY_WARNING(lv_tr("Resume macro not configured"));
            return;
        }

        spdlog::info("[FilamentRunoutHandler] User chose to resume print after runout");

        // Resume via the shared prep+dispatch helper. The AMS backend gets
        // a chance to run any recovery gcode it needs (e.g., Snapmaker U1's
        // post-runout extrude) before the Resume StandardMacro fires.
        // Backends with no prep invoke the dispatch immediately. There's no
        // optimistic-UI state to clean up here, so on_failure is omitted.
        spdlog::info("[FilamentRunoutHandler] Using StandardMacros resume: {}",
                     resume_info.get_macro());
        dispatch_prepared_resume(api_, "[FilamentRunoutHandler]");
    });

    runout_modal_.set_on_cancel_print([this, token]() {
        if (token.expired()) return;

        spdlog::info("[FilamentRunoutHandler] User chose to cancel print after runout");

        // Check if cancel slot is available
        const auto& cancel_info = StandardMacros::instance().get(StandardMacroSlot::Cancel);
        if (cancel_info.is_empty()) {
            spdlog::warn("[FilamentRunoutHandler] Cancel macro slot is empty");
            NOTIFY_WARNING(lv_tr("Cancel macro not configured"));
            return;
        }

        // Cancel the print via StandardMacros
        if (api_) {
            spdlog::info("[FilamentRunoutHandler] Using StandardMacros cancel: {}",
                         cancel_info.get_macro());
            StandardMacros::instance().execute(
                StandardMacroSlot::Cancel, api_,
                []() { spdlog::info("[FilamentRunoutHandler] Print cancelled after runout"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[FilamentRunoutHandler] Failed to cancel print: {}",
                                  err.message);
                    NOTIFY_ERROR(lv_tr("Failed to cancel: {}"), err.user_message());
                });
        }
    });

    runout_modal_.set_on_unload_filament([this, token]() {
        if (token.expired()) return;
        user_took_manual_action_ = true; // in-dialog action suppresses auto-close

        spdlog::info("[FilamentRunoutHandler] User chose to unload filament after runout");

        const auto& unload_info = StandardMacros::instance().get(StandardMacroSlot::UnloadFilament);
        if (unload_info.is_empty()) {
            spdlog::warn("[FilamentRunoutHandler] Unload filament macro slot is empty");
            NOTIFY_WARNING(lv_tr("Unload macro not configured"));
            return;
        }

        if (api_) {
            spdlog::info("[FilamentRunoutHandler] Using StandardMacros unload: {}",
                         unload_info.get_macro());
            StandardMacros::instance().execute(
                StandardMacroSlot::UnloadFilament, api_,
                []() { spdlog::info("[FilamentRunoutHandler] Unload filament started"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[FilamentRunoutHandler] Failed to unload filament: {}",
                                  err.message);
                    NOTIFY_ERROR(lv_tr("Failed to unload: {}"), err.user_message());
                });
        }
    });

    runout_modal_.set_on_purge([this, token]() {
        if (token.expired()) return;
        user_took_manual_action_ = true; // in-dialog action suppresses auto-close

        spdlog::info("[FilamentRunoutHandler] User chose to purge after runout");

        const auto& purge_info = StandardMacros::instance().get(StandardMacroSlot::Purge);
        if (purge_info.is_empty()) {
            spdlog::warn("[FilamentRunoutHandler] Purge macro slot is empty");
            NOTIFY_WARNING(lv_tr("Purge macro not configured"));
            return;
        }

        if (api_) {
            spdlog::info("[FilamentRunoutHandler] Using StandardMacros purge: {}",
                         purge_info.get_macro());
            StandardMacros::instance().execute(
                StandardMacroSlot::Purge, api_,
                []() { spdlog::info("[FilamentRunoutHandler] Purge started"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[FilamentRunoutHandler] Failed to purge: {}", err.message);
                    NOTIFY_ERROR(lv_tr("Failed to purge: {}"), err.user_message());
                });
        }
    });

    runout_modal_.set_on_ok_dismiss([token]() {
        if (token.expired()) return;
        spdlog::info("[FilamentRunoutHandler] User dismissed runout modal (idle mode)");
        // Just hide the modal - no action needed
    });

    if (!runout_modal_.show(lv_screen_active())) {
        spdlog::error("[FilamentRunoutHandler] Failed to create runout guidance modal");
        return;
    }

    // Auto-close when the runout resolves EXTERNALLY. get_any_runout_subject() is
    // int: 1=runout, 0=clear. observe_int_sync fires its INITIAL read the moment
    // it's installed and again on every change — and the sensor can momentarily
    // read 0 during its startup-grace window (e.g. right after a UI restart),
    // which previously closed the modal immediately (#991). Guards, in order:
    //   - value==1: latch a confirmed active runout for THIS modal (never closes)
    //   - startup-grace window: ignore (sensor not yet stabilized)
    //   - require runout_confirmed_active_: only a genuine confirmed runout→clear
    //     transition observed while this modal is up may auto-close
    //   - !user_took_manual_action_: user managing it in-dialog suppresses close
    // observe_int_sync defers to the main thread; hiding here mirrors the
    // existing on_print_state_changed close path (precedent-safe).
    runout_cleared_observer_ = helix::ui::observe_int_sync<FilamentRunoutHandler>(
        helix::FilamentSensorManager::instance().get_any_runout_subject(), this,
        [](FilamentRunoutHandler* self, int any_runout) {
            if (any_runout != 0) {
                // Confirmed active runout while the modal is up — arm auto-close.
                self->runout_confirmed_active_ = true;
                return;
            }
            // value == 0 (clear): only close on a genuine confirmed runout→clear.
            if (helix::FilamentSensorManager::instance().is_in_startup_grace_period()) {
                // Transient 0 during sensor stabilization — not a real resolution.
                return;
            }
            if (!self->runout_confirmed_active_) return; // never saw a confirmed runout
            if (self->user_took_manual_action_) return;  // user is managing it in-dialog
            if (!self->runout_modal_.is_visible()) return;
            spdlog::info(
                "[FilamentRunoutHandler] Runout cleared externally — auto-closing guidance modal");
            self->hide_runout_guidance_modal();
        });
}

void FilamentRunoutHandler::hide_modal() {
    hide_runout_guidance_modal();
}

void FilamentRunoutHandler::hide_runout_guidance_modal() {
    if (!runout_modal_.is_visible()) {
        return;
    }

    spdlog::debug("[FilamentRunoutHandler] Hiding runout guidance modal");
    runout_modal_.hide();
    // Stop observing the runout-cleared subject so it doesn't linger across pauses.
    runout_cleared_observer_.reset();
}

} // namespace helix::ui

// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_guidance_char.cpp
 * @brief Characterization tests for runout guidance functionality in PrintStatusPanel
 *
 * These tests document the EXISTING behavior of the runout guidance feature.
 * Run with: ./build/bin/helix-tests "[runout_guidance][char]"
 *
 * Feature flow:
 * 1. Print pauses (Moonraker sends pause due to runout)
 * 2. on_print_state_changed() detects Paused state
 * 3. check_and_show_runout_guidance() checks guards and shows modal
 * 4. User clicks one of 6 buttons -> action executed
 * 5. Print resumes -> flag reset, modal hidden
 *
 * Key state:
 * - runout_modal_shown_for_pause_ : bool flag preventing duplicate shows
 * - RunoutGuidanceModal runout_modal_ : the modal (already extracted)
 *
 * Guards in check_and_show_runout_guidance():
 * 1. runout_modal_shown_for_pause_ must be false
 * 2. RuntimeConfig::should_show_runout_modal() must be true
 * 3. FilamentSensorManager::has_any_runout() must be true
 */

#include <string>
#include <unordered_set>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Helper Classes - Mirror runout guidance state management logic
// ============================================================================

/**
 * @brief Simulates the runout guidance state machine from PrintStatusPanel
 *
 * This helper mirrors the state transitions and validation logic without
 * requiring the full PrintStatusPanel/LVGL infrastructure.
 */
class RunoutGuidanceStateMachine {
  public:
    // Simulated external state
    struct ExternalState {
        bool runtime_config_allows = true;  // RuntimeConfig::should_show_runout_modal()
        bool has_any_runout = false;        // FilamentSensorManager::has_any_runout()
        bool resume_macro_available = true; // StandardMacros Resume slot not empty
        bool cancel_macro_available = true; // StandardMacros Cancel slot not empty
        bool unload_macro_available = true; // StandardMacros UnloadFilament slot not empty
        bool purge_macro_available = true;  // StandardMacros Purge slot not empty
        bool ams_backend_present = true;    // AmsState::instance().get_backend() != nullptr
    };

    enum class PrintState { Idle, Preparing, Printing, Paused, Complete, Cancelled, Error };

    enum class ActionResult { SUCCESS, BLOCKED_NO_FILAMENT, BLOCKED_NO_MACRO, MODAL_NOT_VISIBLE };

    /**
     * @brief Handle print state change
     *
     * Mirrors on_print_state_changed() logic:
     * - Transition to Paused -> check_and_show_runout_guidance()
     * - Transition to Printing -> reset flag, hide modal
     */
    void on_state_changed(PrintState old_state, PrintState new_state) {
        if (new_state == PrintState::Paused) {
            check_and_show_runout_guidance();
        }

        if (new_state == PrintState::Printing) {
            runout_modal_shown_for_pause_ = false;
            user_took_manual_action_ = false;
            modal_visible_ = false;
        }

        current_state_ = new_state;
    }

    /**
     * @brief Check guards and show runout guidance modal
     *
     * Mirrors check_and_show_runout_guidance() logic:
     * - Skip if already shown for this pause
     * - Skip if RuntimeConfig suppresses (wizard mode, AMS/MMU)
     * - Skip if no runout detected
     */
    void check_and_show_runout_guidance() {
        // Guard 1: Only show once per pause event
        if (runout_modal_shown_for_pause_) {
            return;
        }

        // Guard 2: RuntimeConfig suppression (wizard, AMS/MMU)
        if (!external_state_.runtime_config_allows) {
            return;
        }

        // Guard 3: Check if any runout sensor shows no filament
        if (external_state_.has_any_runout) {
            show_runout_guidance_modal();
            runout_modal_shown_for_pause_ = true;
        }
    }

    /**
     * @brief Show the runout guidance modal
     */
    void show_runout_guidance_modal() {
        if (modal_visible_) {
            return; // Already showing
        }
        user_took_manual_action_ = false; // fresh modal re-arms auto-close
        runout_confirmed_active_ = false; // #991: latch re-armed per fresh modal
        modal_visible_ = true;
    }

    /**
     * @brief Hide the runout guidance modal
     */
    void hide_runout_guidance_modal() {
        modal_visible_ = false;
    }

    /**
     * @brief Mirror the get_any_runout_subject() auto-close observer (#991).
     *
     * Called when the runout-cleared subject fires. Auto-closes the modal only
     * on a GENUINE confirmed-runout→clear transition observed while the modal is
     * up, and only when the runout cleared EXTERNALLY (no in-dialog action).
     *
     * Guards (matching the real observer):
     *  - value==1: latch runout_confirmed_active_ (never closes).
     *  - in_startup_grace: a transient 0 during sensor stabilization is ignored.
     *  - require runout_confirmed_active_: the observer's initial read / any
     *    startup-transient 0 BEFORE a confirmed 1 cannot close the modal.
     *  - user_took_manual_action_: in-dialog action keeps the dialog open.
     *
     * @param any_runout New subject value (1=runout, 0=clear)
     * @param in_startup_grace True if the sensor is still within its grace window
     */
    void on_any_runout_changed(int any_runout, bool in_startup_grace = false) {
        if (any_runout != 0) {
            runout_confirmed_active_ = true; // confirmed active runout — arm close
            return;
        }
        if (in_startup_grace)
            return; // transient 0 during sensor stabilization
        if (!runout_confirmed_active_)
            return; // never saw a confirmed runout for this modal
        if (user_took_manual_action_)
            return; // user is managing it in-dialog
        if (!modal_visible_)
            return;
        modal_visible_ = false; // runout cleared externally — auto-close
    }

    // ========================================================================
    // Action Handlers (mirror show_runout_guidance_modal() callbacks)
    // ========================================================================

    /**
     * @brief Handle "Load Filament" button
     *
     * #991: Loads filament in place (via the AMS backend) and KEEPS the modal
     * open so the user can purge before resuming. Marks a manual action so the
     * external-resolution auto-close is suppressed (the load itself makes the
     * sensor read present). With no AMS backend, falls back to navigating to the
     * filament panel.
     *
     * @return true if the action started
     */
    bool handle_load_filament() {
        if (!modal_visible_)
            return false;

        user_took_manual_action_ = true;
        if (external_state_.ams_backend_present) {
            // Load in place via the backend.
            macro_executed_ = "LoadFilament";
        } else {
            // No AMS backend — fall back to navigating to the filament panel.
            navigated_to_panel_ = "filament";
        }
        // Modal stays open - user may want to purge after loading before resuming
        return true;
    }

    /**
     * @brief Handle "Unload Filament" button
     *
     * Executes UnloadFilament macro. Modal stays open.
     *
     * @return Result of the action
     */
    ActionResult handle_unload_filament() {
        if (!modal_visible_)
            return ActionResult::MODAL_NOT_VISIBLE;

        if (!external_state_.unload_macro_available) {
            notification_shown_ = "Unload macro not configured";
            return ActionResult::BLOCKED_NO_MACRO;
        }

        // Execute macro
        user_took_manual_action_ = true;
        macro_executed_ = "UnloadFilament";
        // Modal stays open
        return ActionResult::SUCCESS;
    }

    /**
     * @brief Handle "Purge" button
     *
     * Executes Purge macro. Modal stays open.
     *
     * @return Result of the action
     */
    ActionResult handle_purge() {
        if (!modal_visible_)
            return ActionResult::MODAL_NOT_VISIBLE;

        if (!external_state_.purge_macro_available) {
            notification_shown_ = "Purge macro not configured";
            return ActionResult::BLOCKED_NO_MACRO;
        }

        // Execute macro
        user_took_manual_action_ = true;
        macro_executed_ = "Purge";
        // Modal stays open
        return ActionResult::SUCCESS;
    }

    /**
     * @brief Handle "Resume" button
     *
     * Checks filament present first, then executes Resume macro.
     *
     * @return Result of the action
     */
    ActionResult handle_resume() {
        if (!modal_visible_)
            return ActionResult::MODAL_NOT_VISIBLE;

        // Check if filament is now present
        if (external_state_.has_any_runout) {
            notification_shown_ = "Insert filament before resuming";
            return ActionResult::BLOCKED_NO_FILAMENT;
        }

        if (!external_state_.resume_macro_available) {
            notification_shown_ = "Resume macro not configured";
            return ActionResult::BLOCKED_NO_MACRO;
        }

        // Execute Resume macro
        macro_executed_ = "Resume";
        // Modal hides
        modal_visible_ = false;
        return ActionResult::SUCCESS;
    }

    /**
     * @brief Handle "Cancel Print" button
     *
     * Executes Cancel macro.
     *
     * @return Result of the action
     */
    ActionResult handle_cancel_print() {
        if (!modal_visible_)
            return ActionResult::MODAL_NOT_VISIBLE;

        if (!external_state_.cancel_macro_available) {
            notification_shown_ = "Cancel macro not configured";
            return ActionResult::BLOCKED_NO_MACRO;
        }

        // Execute Cancel macro
        macro_executed_ = "Cancel";
        // Modal hides
        modal_visible_ = false;
        return ActionResult::SUCCESS;
    }

    /**
     * @brief Handle "OK" dismiss button
     *
     * Just hides the modal, no action.
     *
     * @return true if modal was visible
     */
    bool handle_ok_dismiss() {
        if (!modal_visible_)
            return false;

        modal_visible_ = false;
        return true;
    }

    // Accessors for testing
    bool is_modal_visible() const {
        return modal_visible_;
    }
    bool was_shown_for_pause() const {
        return runout_modal_shown_for_pause_;
    }
    PrintState current_state() const {
        return current_state_;
    }
    const std::string& last_macro_executed() const {
        return macro_executed_;
    }
    const std::string& last_notification() const {
        return notification_shown_;
    }
    const std::string& navigated_panel() const {
        return navigated_to_panel_;
    }
    bool user_took_manual_action() const {
        return user_took_manual_action_;
    }

    // External state control for testing
    ExternalState& external_state() {
        return external_state_;
    }

    void reset() {
        runout_modal_shown_for_pause_ = false;
        user_took_manual_action_ = false;
        runout_confirmed_active_ = false;
        modal_visible_ = false;
        current_state_ = PrintState::Idle;
        macro_executed_.clear();
        notification_shown_.clear();
        navigated_to_panel_.clear();
        external_state_ = ExternalState{};
    }

  private:
    bool runout_modal_shown_for_pause_ = false;
    bool user_took_manual_action_ = false;
    bool runout_confirmed_active_ = false;
    bool modal_visible_ = false;
    PrintState current_state_ = PrintState::Idle;
    std::string macro_executed_;
    std::string notification_shown_;
    std::string navigated_to_panel_;
    ExternalState external_state_;
};

// ============================================================================
// CHARACTERIZATION: Modal Show/Hide Guards
// ============================================================================

TEST_CASE("CHAR: Pause with runout shows modal", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    SECTION("Transition to Paused with runout shows modal") {
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);

        REQUIRE(state.is_modal_visible() == true);
        REQUIRE(state.was_shown_for_pause() == true);
    }
}

TEST_CASE("CHAR: Pause without runout does not show modal", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = false;

    SECTION("Transition to Paused without runout does not show modal") {
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);

        REQUIRE(state.is_modal_visible() == false);
        REQUIRE(state.was_shown_for_pause() == false);
    }
}

TEST_CASE("CHAR: Flag prevents duplicate modal shows during same pause",
          "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // First pause shows modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    // Manually hide modal
    state.hide_runout_guidance_modal();
    REQUIRE(state.is_modal_visible() == false);

    SECTION("Second check_and_show does not show modal again") {
        state.check_and_show_runout_guidance();

        REQUIRE(state.is_modal_visible() == false);
        REQUIRE(state.was_shown_for_pause() == true);
    }
}

TEST_CASE("CHAR: Flag reset when transitioning to Printing", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal on pause
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.was_shown_for_pause() == true);

    SECTION("Flag is reset when print resumes") {
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Paused,
                               RunoutGuidanceStateMachine::PrintState::Printing);

        REQUIRE(state.was_shown_for_pause() == false);
    }
}

TEST_CASE("CHAR: Modal hidden when transitioning to Printing", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal on pause
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    SECTION("Modal is hidden when print resumes") {
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Paused,
                               RunoutGuidanceStateMachine::PrintState::Printing);

        REQUIRE(state.is_modal_visible() == false);
    }
}

TEST_CASE("CHAR: RuntimeConfig suppression works", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;
    state.external_state().runtime_config_allows = false;

    SECTION("Modal not shown when RuntimeConfig suppresses (wizard mode)") {
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);

        REQUIRE(state.is_modal_visible() == false);
        REQUIRE(state.was_shown_for_pause() == false);
    }
}

TEST_CASE("CHAR: Multiple pause/resume cycles work correctly", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    SECTION("Modal shows on each new pause after resume") {
        // First pause
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);
        REQUIRE(state.is_modal_visible() == true);

        // Resume
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Paused,
                               RunoutGuidanceStateMachine::PrintState::Printing);
        REQUIRE(state.is_modal_visible() == false);
        REQUIRE(state.was_shown_for_pause() == false);

        // Second pause
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);
        REQUIRE(state.is_modal_visible() == true);
        REQUIRE(state.was_shown_for_pause() == true);
    }
}

// ============================================================================
// CHARACTERIZATION: Resume Button
// ============================================================================

TEST_CASE("CHAR: Resume blocked if filament still missing", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Resume shows notification when filament still missing") {
        auto result = state.handle_resume();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::BLOCKED_NO_FILAMENT);
        REQUIRE(state.last_notification() == "Insert filament before resuming");
        REQUIRE(state.is_modal_visible() == true); // Modal stays open
        REQUIRE(state.last_macro_executed().empty());
    }
}

TEST_CASE("CHAR: Resume blocked if Resume macro empty", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    // Filament now present, but no Resume macro
    state.external_state().has_any_runout = false;
    state.external_state().resume_macro_available = false;

    SECTION("Resume shows notification when macro not configured") {
        auto result = state.handle_resume();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::BLOCKED_NO_MACRO);
        REQUIRE(state.last_notification() == "Resume macro not configured");
        REQUIRE(state.is_modal_visible() == true); // Modal stays open
    }
}

TEST_CASE("CHAR: Resume executes macro when conditions met", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    // Filament now present
    state.external_state().has_any_runout = false;

    SECTION("Resume executes macro and hides modal") {
        auto result = state.handle_resume();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
        REQUIRE(state.last_macro_executed() == "Resume");
        REQUIRE(state.is_modal_visible() == false); // Modal hidden
    }
}

// ============================================================================
// CHARACTERIZATION: Cancel Print Button
// ============================================================================

TEST_CASE("CHAR: Cancel Print executes Cancel macro", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Cancel Print executes macro and hides modal") {
        auto result = state.handle_cancel_print();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
        REQUIRE(state.last_macro_executed() == "Cancel");
        REQUIRE(state.is_modal_visible() == false);
    }
}

TEST_CASE("CHAR: Cancel Print blocked if Cancel macro empty", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;
    state.external_state().cancel_macro_available = false;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Cancel shows notification when macro not configured") {
        auto result = state.handle_cancel_print();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::BLOCKED_NO_MACRO);
        REQUIRE(state.last_notification() == "Cancel macro not configured");
        REQUIRE(state.is_modal_visible() == true);
    }
}

// ============================================================================
// CHARACTERIZATION: Unload Filament Button
// ============================================================================

TEST_CASE("CHAR: Unload Filament executes macro, modal stays open", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Unload executes macro and modal remains visible") {
        auto result = state.handle_unload_filament();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
        REQUIRE(state.last_macro_executed() == "UnloadFilament");
        REQUIRE(state.is_modal_visible() == true); // Modal stays open
    }
}

TEST_CASE("CHAR: Unload Filament blocked if macro empty", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;
    state.external_state().unload_macro_available = false;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Unload shows notification when macro not configured") {
        auto result = state.handle_unload_filament();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::BLOCKED_NO_MACRO);
        REQUIRE(state.last_notification() == "Unload macro not configured");
        REQUIRE(state.is_modal_visible() == true);
    }
}

// ============================================================================
// CHARACTERIZATION: Purge Button
// ============================================================================

TEST_CASE("CHAR: Purge executes macro, modal stays open", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Purge executes macro and modal remains visible") {
        auto result = state.handle_purge();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
        REQUIRE(state.last_macro_executed() == "Purge");
        REQUIRE(state.is_modal_visible() == true); // Modal stays open
    }
}

TEST_CASE("CHAR: Purge blocked if macro empty", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;
    state.external_state().purge_macro_available = false;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("Purge shows notification when macro not configured") {
        auto result = state.handle_purge();

        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::BLOCKED_NO_MACRO);
        REQUIRE(state.last_notification() == "Purge macro not configured");
        REQUIRE(state.is_modal_visible() == true);
    }
}

// ============================================================================
// CHARACTERIZATION: Load Filament Button
// ============================================================================

TEST_CASE("CHAR: Load Filament loads in place and keeps modal open",
          "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("With AMS backend: loads in place, modal stays open, manual flag set") {
        state.external_state().ams_backend_present = true;
        bool result = state.handle_load_filament();

        REQUIRE(result == true);
        REQUIRE(state.last_macro_executed() == "LoadFilament");
        REQUIRE(state.navigated_panel().empty());
        REQUIRE(state.is_modal_visible() == true); // #991: stays open for follow-up purge
        REQUIRE(state.user_took_manual_action() == true);
    }

    SECTION("Without AMS backend: falls back to navigating, modal stays open") {
        state.external_state().ams_backend_present = false;
        bool result = state.handle_load_filament();

        REQUIRE(result == true);
        REQUIRE(state.navigated_panel() == "filament");
        REQUIRE(state.is_modal_visible() == true);
        REQUIRE(state.user_took_manual_action() == true);
    }
}

// ============================================================================
// CHARACTERIZATION: OK Dismiss Button
// ============================================================================

TEST_CASE("CHAR: OK dismiss just hides modal", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);

    SECTION("OK dismiss hides modal with no other action") {
        bool result = state.handle_ok_dismiss();

        REQUIRE(result == true);
        REQUIRE(state.is_modal_visible() == false);
        REQUIRE(state.last_macro_executed().empty());
        REQUIRE(state.navigated_panel().empty());
    }
}

// ============================================================================
// CHARACTERIZATION: Edge Cases
// ============================================================================

TEST_CASE("CHAR: Actions fail when modal not visible", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Don't show modal
    REQUIRE(state.is_modal_visible() == false);

    SECTION("Resume fails when modal not visible") {
        auto result = state.handle_resume();
        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::MODAL_NOT_VISIBLE);
    }

    SECTION("Cancel Print fails when modal not visible") {
        auto result = state.handle_cancel_print();
        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::MODAL_NOT_VISIBLE);
    }

    SECTION("Unload Filament fails when modal not visible") {
        auto result = state.handle_unload_filament();
        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::MODAL_NOT_VISIBLE);
    }

    SECTION("Purge fails when modal not visible") {
        auto result = state.handle_purge();
        REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::MODAL_NOT_VISIBLE);
    }

    SECTION("Load Filament fails when modal not visible") {
        bool result = state.handle_load_filament();
        REQUIRE(result == false);
    }

    SECTION("OK dismiss fails when modal not visible") {
        bool result = state.handle_ok_dismiss();
        REQUIRE(result == false);
    }
}

TEST_CASE("CHAR: Show modal is idempotent", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;
    state.external_state().has_any_runout = true;

    // Show modal
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    SECTION("Calling show again doesn't crash or change state") {
        state.show_runout_guidance_modal();
        REQUIRE(state.is_modal_visible() == true);
    }
}

TEST_CASE("CHAR: Hide modal is idempotent", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    SECTION("Calling hide on non-visible modal is safe") {
        REQUIRE(state.is_modal_visible() == false);
        state.hide_runout_guidance_modal();
        REQUIRE(state.is_modal_visible() == false);
    }

    SECTION("Double hide is safe") {
        state.external_state().has_any_runout = true;
        state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                               RunoutGuidanceStateMachine::PrintState::Paused);

        state.hide_runout_guidance_modal();
        state.hide_runout_guidance_modal();
        REQUIRE(state.is_modal_visible() == false);
    }
}

// ============================================================================
// CHARACTERIZATION: Full Workflow Scenarios
// ============================================================================

TEST_CASE("CHAR: Complete runout workflow - load and resume", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Step 1: Print is running, runout detected, printer pauses
    state.external_state().has_any_runout = true;
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    // Step 2: User clicks Load Filament. #991: loads in place via the backend,
    // marks a manual action, and the modal STAYS OPEN for a follow-up purge.
    state.handle_load_filament();
    REQUIRE(state.last_macro_executed() == "LoadFilament");
    REQUIRE(state.is_modal_visible() == true);
    REQUIRE(state.user_took_manual_action() == true);

    // Step 3: The load makes the sensor read present. The auto-close observer
    // fires, but because the user took an in-dialog action it does NOT close.
    state.external_state().has_any_runout = false;
    state.on_any_runout_changed(0);
    REQUIRE(state.is_modal_visible() == true); // suppressed by manual-action flag

    // Step 4: User resumes from the still-open dialog.
    auto result = state.handle_resume();
    REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
    REQUIRE(state.last_macro_executed() == "Resume");
    REQUIRE(state.is_modal_visible() == false);
}

TEST_CASE("CHAR: Runout cleared externally auto-closes modal", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Step 1: Runout detected, modal shown.
    state.external_state().has_any_runout = true;
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);
    REQUIRE(state.user_took_manual_action() == false);

    SECTION("External resolution (no in-dialog action) auto-closes") {
        // Observer confirms the active runout (value==1) while the modal is up,
        // then the user loads filament via some OTHER path; sensor reads present.
        state.on_any_runout_changed(1);
        state.external_state().has_any_runout = false;
        state.on_any_runout_changed(0);
        REQUIRE(state.is_modal_visible() == false);
    }

    SECTION("Still-in-runout subject value does not close") {
        state.on_any_runout_changed(1);
        REQUIRE(state.is_modal_visible() == true);
    }

    SECTION("In-dialog action suppresses auto-close") {
        state.on_any_runout_changed(1); // confirmed active runout
        state.handle_purge();
        REQUIRE(state.user_took_manual_action() == true);
        state.external_state().has_any_runout = false;
        state.on_any_runout_changed(0);
        REQUIRE(state.is_modal_visible() == true); // stays open
    }
}

// ============================================================================
// CHARACTERIZATION: Auto-close latch (#991) — startup-grace misfire guard
// ============================================================================

TEST_CASE("CHAR: Initial-read 0 does not close modal without a confirmed runout",
          "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Runout detected, modal shown.
    state.external_state().has_any_runout = true;
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    SECTION("Observer's initial 0 read (before any confirmed runout) is ignored") {
        // This is the Phase 2 bug: observer fires its initial read as 0 right
        // after install (UI restart / startup transient) and used to close the
        // modal immediately. The latch requires a confirmed runout first.
        state.on_any_runout_changed(0);
        REQUIRE(state.is_modal_visible() == true);
    }

    SECTION("Transient 0 during startup-grace window is ignored even after confirm") {
        state.on_any_runout_changed(1); // confirmed active runout
        // A grace-window transient 0 must not close (sensor not yet stabilized).
        state.on_any_runout_changed(0, /*in_startup_grace=*/true);
        REQUIRE(state.is_modal_visible() == true);
        // Once grace ends, a genuine clear DOES close.
        state.on_any_runout_changed(0, /*in_startup_grace=*/false);
        REQUIRE(state.is_modal_visible() == false);
    }
}

TEST_CASE("CHAR: Complete runout workflow - purge multiple times then resume",
          "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Step 1: Runout detected, modal shown
    state.external_state().has_any_runout = true;
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    // Step 2: User clicks Purge multiple times
    state.handle_purge();
    REQUIRE(state.last_macro_executed() == "Purge");
    REQUIRE(state.is_modal_visible() == true);

    state.handle_purge();
    REQUIRE(state.last_macro_executed() == "Purge");
    REQUIRE(state.is_modal_visible() == true);

    // Step 3: User inserts filament
    state.external_state().has_any_runout = false;

    // Step 4: User clicks Resume
    auto result = state.handle_resume();
    REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
    REQUIRE(state.last_macro_executed() == "Resume");
    REQUIRE(state.is_modal_visible() == false);
}

TEST_CASE("CHAR: Complete runout workflow - cancel print", "[runout_guidance][char]") {
    RunoutGuidanceStateMachine state;

    // Step 1: Runout detected, modal shown
    state.external_state().has_any_runout = true;
    state.on_state_changed(RunoutGuidanceStateMachine::PrintState::Printing,
                           RunoutGuidanceStateMachine::PrintState::Paused);
    REQUIRE(state.is_modal_visible() == true);

    // Step 2: User decides to cancel print
    auto result = state.handle_cancel_print();
    REQUIRE(result == RunoutGuidanceStateMachine::ActionResult::SUCCESS);
    REQUIRE(state.last_macro_executed() == "Cancel");
    REQUIRE(state.is_modal_visible() == false);
}

// ============================================================================
// Documentation: Runout Guidance Pattern Summary
// ============================================================================

/**
 * SUMMARY OF RUNOUT GUIDANCE CHARACTERIZATION:
 *
 * State Machine:
 * - IDLE: No modal visible, no pending pause
 * - PAUSED_NO_MODAL: Paused but no runout (or suppressed)
 * - PAUSED_MODAL_SHOWN: Paused with runout, modal visible
 *
 * Guards for showing modal:
 * 1. runout_modal_shown_for_pause_ == false
 * 2. RuntimeConfig::should_show_runout_modal() == true
 * 3. FilamentSensorManager::has_any_runout() == true
 *
 * State Transitions:
 * - Printing -> Paused: check_and_show_runout_guidance()
 * - Paused -> Printing: reset flag, hide modal
 *
 * Button Actions:
 * 1. Load Filament: navigate to filament panel, modal hides
 * 2. Unload Filament: execute macro, modal stays open
 * 3. Purge: execute macro, modal stays open
 * 4. Resume: check filament + macro, execute, modal hides
 * 5. Cancel Print: execute macro, modal hides
 * 6. OK: dismiss modal, no action
 *
 * Resume Validation:
 * - Checks has_any_runout() first - blocks with notification if still missing
 * - Checks Resume macro availability - blocks with notification if empty
 *
 * Key Behaviors:
 * - Modal only shown once per pause (flag prevents duplicates)
 * - Flag reset only when transitioning to Printing
 * - RuntimeConfig suppression for wizard mode, AMS/MMU
 * - Unload/Purge don't hide modal (user may need multiple operations)
 * - All actions are no-ops if modal not visible
 */

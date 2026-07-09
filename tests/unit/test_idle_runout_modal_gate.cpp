// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_idle_runout_modal_gate.cpp
 * @brief Gating tests for the IDLE filament-runout modal in PrintStatusWidget.
 *
 * Run with: ./build/bin/helix-tests "[idle_runout][gate]"
 *
 * Bug (Snapmaker U1, live): while the printer was IDLE the user manually
 * UNLOADED a filament lane. The lane's runout sensor went empty, the
 * `any_runout` subject flipped to 1, the PrintStatusWidget observer ran
 * check_and_show_idle_runout_modal(), and a "filament runout" modal popped
 * up. A manual unload while idle must NOT raise a runout alarm.
 *
 * The toast path (FilamentSensorManager) already suppresses spurious sensor
 * triggers during an active AMS load/unload via
 * AmsState::is_filament_operation_active(). The idle-runout MODAL path did
 * not. This helper mirrors check_and_show_idle_runout_modal()'s guard chain
 * so the gating can be unit-tested without LVGL / modal / navigation
 * infrastructure. The real method's logic must stay in lockstep with this.
 */

#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Helper: mirrors PrintStatusWidget::check_and_show_idle_runout_modal() guards
// ============================================================================

/**
 * @brief Simulates the idle-runout-modal gating chain.
 *
 * Guard order (matches the real method):
 *   1. startup grace period      -> skip
 *   2. has_any_runout == false   -> skip (sensor not actually empty)
 *   3. runtime config suppresses -> skip (AMS-no-bypass / wizard)
 *   4. AMS filament op active     -> skip  (THE FIX — manual load/unload moves
 *                                           filament past sensors intentionally)
 *   5. modal already shown       -> skip
 *   6. print active (printing/   -> skip  (this is the "idle" modal; an active
 *      paused)                            print is handled by the pause-gated
 *                                         FilamentRunoutHandler instead)
 *   else -> show modal.
 */
class IdleRunoutModalGate {
  public:
    enum class PrintState { Standby, Printing, Paused, Complete, Cancelled, Error };

    struct ExternalState {
        bool in_startup_grace = false;       // FilamentSensorManager::is_in_startup_grace_period()
        bool has_any_runout = true;          // FilamentSensorManager::has_any_runout()
        bool runtime_config_allows = true;   // RuntimeConfig::should_show_runout_modal()
        bool ams_filament_op_active = false; // AmsState::is_filament_operation_active()
        PrintState print_state = PrintState::Standby;
    };

    void check_and_show_idle_runout_modal() {
        if (ext_.in_startup_grace)
            return;
        if (!ext_.has_any_runout)
            return;
        if (!ext_.runtime_config_allows)
            return;
        // THE FIX: a manual load/unload deliberately drags filament past the
        // runout sensor — the resulting empty reading is not a real runout.
        if (ext_.ams_filament_op_active)
            return;
        if (modal_shown_)
            return;
        // This modal is the IDLE-only variant. While a print is active
        // (printing/paused) the pause-gated FilamentRunoutHandler owns runout.
        if (ext_.print_state != PrintState::Standby && ext_.print_state != PrintState::Complete &&
            ext_.print_state != PrintState::Cancelled)
            return;

        modal_shown_ = true;
        modal_visible_ = true;
    }

    // Mirrors the `else` branch of the any_runout observer: clearing resets the
    // one-shot flag so a later genuine runout can show again.
    void on_any_runout_cleared() {
        modal_shown_ = false;
    }

    bool is_modal_visible() const {
        return modal_visible_;
    }
    bool was_shown() const {
        return modal_shown_;
    }
    ExternalState& ext() {
        return ext_;
    }

  private:
    bool modal_shown_ = false;
    bool modal_visible_ = false;
    ExternalState ext_;
};

// ============================================================================
// The bug + the fix
// ============================================================================

TEST_CASE("Idle manual unload does not show runout modal", "[idle_runout][gate]") {
    IdleRunoutModalGate g;
    g.ext().print_state = IdleRunoutModalGate::PrintState::Standby;
    g.ext().has_any_runout = true;         // sensor went empty from the manual pull
    g.ext().ams_filament_op_active = true; // AMS is UNLOADING

    g.check_and_show_idle_runout_modal();

    REQUIRE(g.is_modal_visible() == false);
    REQUIRE(g.was_shown() == false);
}

TEST_CASE("Idle genuine empty spool (no AMS op) still shows runout modal", "[idle_runout][gate]") {
    IdleRunoutModalGate g;
    g.ext().print_state = IdleRunoutModalGate::PrintState::Standby;
    g.ext().has_any_runout = true;
    g.ext().ams_filament_op_active = false; // nothing moving filament

    g.check_and_show_idle_runout_modal();

    REQUIRE(g.is_modal_visible() == true);
    REQUIRE(g.was_shown() == true);
}

TEST_CASE("Active print runout is not handled by the idle modal", "[idle_runout][gate]") {
    // The pause-gated FilamentRunoutHandler owns in-print runout. The idle
    // modal must not fire while printing/paused regardless of AMS op state.
    IdleRunoutModalGate g;
    g.ext().has_any_runout = true;

    SECTION("printing") {
        g.ext().print_state = IdleRunoutModalGate::PrintState::Printing;
        g.check_and_show_idle_runout_modal();
        REQUIRE(g.is_modal_visible() == false);
    }
    SECTION("paused") {
        g.ext().print_state = IdleRunoutModalGate::PrintState::Paused;
        g.check_and_show_idle_runout_modal();
        REQUIRE(g.is_modal_visible() == false);
    }
}

TEST_CASE("AMS-op gate does not suppress a genuine in-print runout path", "[idle_runout][gate]") {
    // Conservative-fix guard: the AMS-op gate is layered on top of the existing
    // idle-only gate, so it can never suppress an in-print event (that path is
    // already excluded by print-state). Documenting the layering explicitly.
    IdleRunoutModalGate g;
    g.ext().print_state = IdleRunoutModalGate::PrintState::Printing;
    g.ext().has_any_runout = true;
    g.ext().ams_filament_op_active = false;

    g.check_and_show_idle_runout_modal();
    REQUIRE(g.is_modal_visible() == false); // idle modal stays out of in-print runout
}

TEST_CASE("Other guards still suppress the idle modal", "[idle_runout][gate]") {
    SECTION("startup grace period") {
        IdleRunoutModalGate g;
        g.ext().in_startup_grace = true;
        g.check_and_show_idle_runout_modal();
        REQUIRE(g.is_modal_visible() == false);
    }
    SECTION("no actual runout") {
        IdleRunoutModalGate g;
        g.ext().has_any_runout = false;
        g.check_and_show_idle_runout_modal();
        REQUIRE(g.is_modal_visible() == false);
    }
    SECTION("runtime config suppresses (AMS no-bypass / wizard)") {
        IdleRunoutModalGate g;
        g.ext().runtime_config_allows = false;
        g.check_and_show_idle_runout_modal();
        REQUIRE(g.is_modal_visible() == false);
    }
}

TEST_CASE("Once-only: cleared runout re-arms the modal for a later genuine runout",
          "[idle_runout][gate]") {
    IdleRunoutModalGate g;
    g.ext().has_any_runout = true;

    // Genuine idle empty spool shows once.
    g.check_and_show_idle_runout_modal();
    REQUIRE(g.is_modal_visible() == true);

    // Filament re-inserted -> subject clears -> flag re-armed.
    g.on_any_runout_cleared();
    REQUIRE(g.was_shown() == false);
}

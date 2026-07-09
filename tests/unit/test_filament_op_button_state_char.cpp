// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_op_button_state_char.cpp
 * @brief Characterization tests for FilamentPanel on-button op feedback state
 *
 * Run with: ./build/bin/helix-tests "[filament][op_state][char]"
 *
 * The on-button feedback replaces the old stacked start/complete toasts: each op
 * button shows an animated spinner while its operation runs (state 1), a
 * checkmark on success (state 2) that auto-reverts to idle (state 0), and goes
 * straight back to idle on error/timeout while the error toast still fires.
 *
 * The real FilamentPanel is tightly coupled to LVGL/Moonraker (a g-code round
 * trip with a background-thread success callback), so — matching the established
 * pattern in test_filament_preheat_cooldown_char.cpp and
 * test_filament_bypass_routing_char.cpp — this mirrors the op-state transition
 * rules as a lightweight state machine. It encodes exactly the contract
 * implemented by FilamentPanel::op_started / op_succeeded / op_failed:
 *
 *   - op_started(op): cancel any pending revert, reset every OTHER op to idle,
 *                     set THIS op to 1 (busy). No start toast.
 *   - op_succeeded(op): set op to 2 (done), arm a one-shot revert → 0.
 *   - op_failed(op): cancel revert, set op to 0 (idle). Error toast still fires.
 *
 * These tests FAIL if any of those transitions regress.
 */

#include <array>
#include <optional>

#include "../catch_amalgamated.hpp"

namespace {

enum class Op { Load, Unload, Purge, Extrude, Retract, COUNT };

constexpr uint32_t MIN_SPINNER_VISIBLE_MS = 500;

// Mirror of FilamentPanel's op-state subjects + shared timer + AMS-backend
// completion behaviour. The single `op_timer` handle covers BOTH phases:
//   phase == DoneDelay → the min-spinner floor; firing enters the done state
//   phase == Revert    → the done→idle hold; firing resets the op to idle
class OpButtonStateMachine {
  public:
    enum class TimerPhase { None, DoneDelay, Revert };

    std::array<int, static_cast<size_t>(Op::COUNT)> state{}; // all 0 (idle)
    TimerPhase phase = TimerPhase::None;
    Op revert_target = Op::Load;

    // AMS-backend completion gating (mirrors backend_op_active_ / op_in_flight_).
    bool backend_op_active = false;
    std::optional<Op> op_in_flight;

    uint32_t busy_started_tick = 0;
    uint32_t now = 0; // test-controlled clock

    // Counts on-button feedback writes that would have been toasts before.
    int start_toasts = 0;    // must stay 0 — feature deletes the start toast
    int complete_toasts = 0; // must stay 0 — feature deletes the complete toast
    int error_toasts = 0;    // error/timeout toast is KEPT

    int& st(Op op) {
        return state[static_cast<size_t>(op)];
    }

    void cancel_timer() {
        phase = TimerPhase::None;
    }

    void enter_done_state(Op op) {
        st(op) = 2; // done → checkmark (NO complete toast)
        revert_target = op;
        phase = TimerPhase::Revert; // arm done→idle revert
    }

    void op_started(Op op) {
        cancel_timer(); // cancel any pending min-delay OR revert
        for (size_t i = 0; i < state.size(); ++i) {
            if (i != static_cast<size_t>(op)) {
                state[i] = 0;
            }
        }
        busy_started_tick = now;
        st(op) = 1; // busy → spinner (NO start toast)
    }

    void op_succeeded(Op op) {
        uint32_t elapsed = now - busy_started_tick;
        if (elapsed >= MIN_SPINNER_VISIBLE_MS) {
            enter_done_state(op);
            return;
        }
        revert_target = op;
        phase = TimerPhase::DoneDelay; // hold spinner until the floor elapses
    }

    void op_failed(Op op) {
        cancel_timer(); // clears any pending min-delay or revert
        st(op) = 0;     // back to idle
        ++error_toasts; // error/timeout toast is preserved
    }

    // Mirrors ams_action_observer_: completes a fire-and-forget backend op when
    // the AMS action returns to IDLE. Gated on backend_op_active so it NEVER
    // completes gcode/macro ops.
    void ams_action_idle() {
        if (backend_op_active && op_in_flight) {
            Op op = *op_in_flight;
            backend_op_active = false;
            op_in_flight.reset();
            op_succeeded(op);
        }
    }

    // Simulate the shared timer firing.
    void fire_timer() {
        TimerPhase p = phase;
        phase = TimerPhase::None;
        if (p == TimerPhase::DoneDelay) {
            enter_done_state(revert_target);
        } else if (p == TimerPhase::Revert) {
            state[static_cast<size_t>(revert_target)] = 0;
        }
    }

    // Convenience for tests still using the old name (revert phase only).
    void fire_revert_timer() {
        fire_timer();
    }
};

} // namespace

TEST_CASE("op_started sets busy and emits no start toast", "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Extrude);
    CHECK(sm.st(Op::Extrude) == 1);
    CHECK(sm.start_toasts == 0);
}

TEST_CASE("op_succeeded shows checkmark then reverts to idle, no complete toast",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Load);
    sm.now += MIN_SPINNER_VISIBLE_MS; // spinner already shown long enough
    sm.op_succeeded(Op::Load);
    CHECK(sm.st(Op::Load) == 2); // checkmark held
    CHECK(sm.complete_toasts == 0);
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::Revert);

    sm.fire_timer();
    CHECK(sm.st(Op::Load) == 0); // auto-reverted to idle
    CHECK(sm.phase == OpButtonStateMachine::TimerPhase::None);
}

TEST_CASE("op_failed returns to idle and keeps the error toast", "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Purge);
    REQUIRE(sm.st(Op::Purge) == 1);

    sm.op_failed(Op::Purge);
    CHECK(sm.st(Op::Purge) == 0); // idle, not stuck on a spinner
    CHECK(sm.error_toasts == 1);  // error/timeout toast preserved
}

TEST_CASE("starting a new op resets the previously-active op", "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Load);
    sm.now += MIN_SPINNER_VISIBLE_MS;
    sm.op_succeeded(Op::Load); // Load now showing checkmark (state 2), revert armed
    REQUIRE(sm.st(Op::Load) == 2);
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::Revert);

    // User triggers Unload before Load's checkmark reverts.
    sm.op_started(Op::Unload);
    CHECK(sm.st(Op::Unload) == 1);                             // new op busy
    CHECK(sm.st(Op::Load) == 0);                               // prior op force-reset to idle
    CHECK(sm.phase == OpButtonStateMachine::TimerPhase::None); // stale revert cancelled
}

TEST_CASE("only one op is ever non-idle at a time", "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    for (Op op : {Op::Load, Op::Unload, Op::Purge, Op::Extrude, Op::Retract}) {
        sm.op_started(op);
        int busy_count = 0;
        for (int s : sm.state) {
            if (s != 0) {
                ++busy_count;
            }
        }
        CHECK(busy_count == 1);
        CHECK(sm.st(op) == 1);
    }
}

// ============================================================================
// Minimum spinner visible duration (FIX 2)
// ============================================================================

TEST_CASE("instant op holds the spinner before showing the checkmark",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Extrude);
    // Success fires synchronously (mock gcode): elapsed == 0 < MIN.
    sm.op_succeeded(Op::Extrude);
    CHECK(sm.st(Op::Extrude) == 1); // STILL spinning — not flipped to check yet
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::DoneDelay);

    // After the floor elapses the deferred-done timer fires.
    sm.fire_timer();
    CHECK(sm.st(Op::Extrude) == 2); // now checkmark
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::Revert);

    sm.fire_timer();
    CHECK(sm.st(Op::Extrude) == 0); // reverted to idle
}

TEST_CASE("slow op shows the checkmark immediately (no extra delay)",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Retract);
    sm.now += MIN_SPINNER_VISIBLE_MS + 100; // spinner already visible well past the floor
    sm.op_succeeded(Op::Retract);
    CHECK(sm.st(Op::Retract) == 2); // straight to checkmark, no DoneDelay phase
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::Revert);
}

TEST_CASE("error during the min-spinner hold cancels the deferred checkmark",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.op_started(Op::Purge);
    sm.op_succeeded(Op::Purge); // elapsed 0 → DoneDelay armed
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::DoneDelay);

    sm.op_failed(Op::Purge); // e.g. a later error path
    CHECK(sm.st(Op::Purge) == 0);
    CHECK(sm.phase == OpButtonStateMachine::TimerPhase::None); // no stuck deferred-done
}

// ============================================================================
// AMS-backend Load/Unload completion via ams_action IDLE (FIX 1)
// ============================================================================

TEST_CASE("AMS-backend load drives spinner then checkmark via ams_action IDLE, no toasts",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    // Backend Load branch: start guard + spinner, mark backend op in flight.
    sm.backend_op_active = true;
    sm.op_in_flight = Op::Load;
    sm.op_started(Op::Load);
    CHECK(sm.st(Op::Load) == 1);
    CHECK(sm.start_toasts == 0); // no "Loading..." start toast

    // Backend runs asynchronously; AMS action eventually returns to IDLE.
    sm.now += MIN_SPINNER_VISIBLE_MS + 50; // real backend op takes time
    sm.ams_action_idle();
    CHECK(sm.st(Op::Load) == 2);       // checkmark
    CHECK(sm.complete_toasts == 0);    // no "Filament loaded" toast
    CHECK_FALSE(sm.backend_op_active); // gate cleared
    CHECK_FALSE(sm.op_in_flight.has_value());

    sm.fire_timer();
    CHECK(sm.st(Op::Load) == 0); // reverts to idle
}

TEST_CASE("AMS-backend unload completes via ams_action IDLE with no start toast",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.backend_op_active = true;
    sm.op_in_flight = Op::Unload;
    sm.op_started(Op::Unload);
    CHECK(sm.st(Op::Unload) == 1);
    CHECK(sm.start_toasts == 0); // the removed NOTIFY_INFO("Unloading filament...")

    sm.now += MIN_SPINNER_VISIBLE_MS + 50;
    sm.ams_action_idle();
    CHECK(sm.st(Op::Unload) == 2);
    CHECK(sm.complete_toasts == 0);
    CHECK_FALSE(sm.backend_op_active);
}

TEST_CASE("ams_action IDLE does NOT complete gcode/macro ops (no double-completion)",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    // A gcode/macro op: op_started WITHOUT setting backend_op_active.
    sm.op_started(Op::Purge);
    sm.now += MIN_SPINNER_VISIBLE_MS;
    sm.op_succeeded(Op::Purge); // completes via its own gcode callback → checkmark
    REQUIRE(sm.st(Op::Purge) == 2);
    REQUIRE(sm.phase == OpButtonStateMachine::TimerPhase::Revert);

    // A stray ams_action IDLE must NOT re-complete or disturb the gcode op.
    sm.ams_action_idle();
    CHECK(sm.st(Op::Purge) == 2); // unchanged — still in its own revert phase
    CHECK(sm.phase == OpButtonStateMachine::TimerPhase::Revert);
}

TEST_CASE("AMS-backend immediate failure goes idle and keeps the error toast",
          "[filament][op_state][char]") {
    OpButtonStateMachine sm;
    sm.backend_op_active = true;
    sm.op_in_flight = Op::Load;
    sm.op_started(Op::Load);

    // backend->load_filament returned !success(): clear gate + fail (+ NOTIFY_ERROR).
    sm.backend_op_active = false;
    sm.op_in_flight.reset();
    sm.op_failed(Op::Load);

    CHECK(sm.st(Op::Load) == 0);
    CHECK(sm.error_toasts == 1); // NOTIFY_ERROR kept on backend immediate failure
    CHECK_FALSE(sm.backend_op_active);
    // A subsequent stray IDLE must be a no-op (gate already cleared).
    sm.ams_action_idle();
    CHECK(sm.st(Op::Load) == 0);
}

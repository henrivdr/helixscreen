// tests/unit/test_print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_buttons.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_control_buttons_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "ui_update_queue.h"

#include <lvgl.h>

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct ControlButtonsFixture : LVGLTestFixture {
    ControlButtonsFixture() {
        // Tear the controller down FIRST: its observer is registered on the
        // print_state_enum subject. PrinterStateTestAccess::reset() deinits that
        // subject (freeing its observers in LVGL); resetting the controller
        // observer afterwards would lv_observer_remove() a freed observer (UAF
        // across sequential test cases — the singleton persists).
        helix::ui::PrintControlButtonsTestAccess::reset();

        // Fresh PrinterState subjects so print_state_enum is valid to observe.
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);

        helix::ui::PrintControlButtons::instance().init_subjects();
    }
};

int read_int(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}
std::string read_str(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}
void set_print_state(helix::PrintJobState s) {
    lv_subject_set_int(get_printer_state().get_print_state_enum_subject(), static_cast<int>(s));
    // observe_int_sync defers via queue_update — drain so the controller reacts.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

} // namespace

TEST_CASE_METHOD(ControlButtonsFixture, "controller registers global subjects",
                 "[print_control][slow]") {
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_icon") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_label") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_enabled") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_stop_enabled") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_pending_action") != nullptr);
}

TEST_CASE_METHOD(ControlButtonsFixture, "primary label tracks print state",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_str("print_control_primary_label") == "Resume");
    set_print_state(helix::PrintJobState::STANDBY);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
    REQUIRE(read_int("print_control_primary_enabled") == 0);
    REQUIRE(read_int("print_control_stop_enabled") == 0);
}

// Enable state depends on StandardMacros slot availability. In the unit-test
// harness no printer discovery runs, so the Pause/Resume/Cancel slots are
// EMPTY (FALLBACK_MACROS has "" for all three, and init() is never called).
// The pure enable logic (active && pending==None && slot_ok) is fully covered
// by Task 1's [print_control_view] tests; here we assert the controller wires
// the slot-availability gate through correctly: with empty slots, the primary
// button is disabled even while PRINTING/PAUSED.
TEST_CASE_METHOD(ControlButtonsFixture, "primary disabled when macro slots empty",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    REQUIRE(read_int("print_control_primary_enabled") == 0);
    REQUIRE(read_int("print_control_stop_enabled") == 0);
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_int("print_control_primary_enabled") == 0);
}

// Optimistic pending action drives the icon/label to the "...ing" form and
// publishes the pending_action subject, independent of macro availability.
TEST_CASE_METHOD(ControlButtonsFixture, "pending action publishes hourglass state",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    helix::ui::PrintControlButtonsTestAccess::set_pending(helix::ui::PendingAction::Pausing);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(helix::ui::PendingAction::Pausing));
    REQUIRE(read_str("print_control_primary_label") == "Pausing...");

    // A real state change is authoritative and clears the optimistic pending.
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(helix::ui::PendingAction::None));
    REQUIRE(read_str("print_control_primary_label") == "Resume");
}

// While a pending action is in flight the primary button is disabled (you cannot
// re-trigger Pause while a Pause is mid-flight); the real state arrival clears it.
TEST_CASE_METHOD(ControlButtonsFixture, "pending action set then cleared on state arrival",
                 "[print_control][slow]") {
    using helix::ui::PendingAction;
    using helix::ui::PrintControlButtonsTestAccess;

    set_print_state(helix::PrintJobState::PRINTING);
    PrintControlButtonsTestAccess::set_pending(PendingAction::Pausing);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(PendingAction::Pausing));
    REQUIRE(read_str("print_control_primary_label") == "Pausing...");
    REQUIRE(read_int("print_control_primary_enabled") == 0); // disabled in flight

    set_print_state(helix::PrintJobState::PAUSED); // real state arrives -> observer clears pending
    REQUIRE(read_int("print_pending_action") == static_cast<int>(PendingAction::None));
    REQUIRE(read_str("print_control_primary_label") == "Resume");
}

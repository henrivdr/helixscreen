// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// ============================================================================
// TestAccess helper — exposes AmsEditModal internals for white-box testing
// ============================================================================

class AmsEditModalTestAccess {
  public:
    explicit AmsEditModalTestAccess(AmsEditModal& modal) : modal_(modal) {}

    void set_original_info(const SlotInfo& info) {
        modal_.original_info_ = info;
    }
    void set_working_info(const SlotInfo& info) {
        modal_.working_info_ = info;
    }
    void set_api(MoonrakerAPI* api) {
        modal_.api_ = api;
    }
    void set_slot_index(int idx) {
        modal_.slot_index_ = idx;
    }
    void set_filament_user_edited(bool edited) {
        modal_.filament_user_edited_ = edited;
    }

    void init_subjects() {
        modal_.init_subjects();
    }

    // View mode subject: 0 = form view, 1 = Spoolman picker view.
    int get_view_mode() {
        return lv_subject_get_int(&modal_.view_mode_subject_);
    }
    void call_handle_save() {
        modal_.handle_save();
    }

    void set_completion_callback(AmsEditModal::CompletionCallback cb) {
        modal_.completion_callback_ = std::move(cb);
    }

    // Forward the private static create-gate predicate (friend access).
    static bool should_create_new_spool(const SlotInfo& working_info, bool filament_user_edited) {
        return AmsEditModal::should_create_new_spool(working_info, filament_user_edited);
    }
    static bool is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
        return AmsEditModal::is_material_identity_change(original, edited);
    }

  private:
    AmsEditModal& modal_;
};

// ============================================================================
// Tests: handle_save syncs active spool with Moonraker
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "handle_save sets active spool when spool assigned (0 -> N)",
                 "[ams_edit_modal][spoolman][active_spool]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.spoolman_mock().set_active_spool(0, nullptr, nullptr);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 0);

    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo working;
    working.spoolman_id = 42;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditModal::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "handle_save sets active spool when spool changed (N -> M)",
                 "[ams_edit_modal][spoolman][active_spool]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.spoolman_mock().set_active_spool(42, nullptr, nullptr);

    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 99;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback(
        [&](const AmsEditModal::EditResult&) { completion_fired = true; });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 99);
}

TEST_CASE_METHOD(LVGLTestFixture, "handle_save clears active spool when spool unlinked (N -> 0)",
                 "[ams_edit_modal][spoolman][active_spool]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    api.spoolman_mock().set_active_spool(42, nullptr, nullptr);

    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 0;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback(
        [&](const AmsEditModal::EditResult&) { completion_fired = true; });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "handle_save re-syncs active spool on unchanged linked save",
                 "[ams_edit_modal][spoolman][active_spool]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Simulate Moonraker having lost the active-spool state (e.g. after restart).
    api.spoolman_mock().set_active_spool(7, nullptr, nullptr);

    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 42;

    SlotInfo working;
    working.spoolman_id = 42; // Same spool — re-save, not a change

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(&api);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback(
        [&](const AmsEditModal::EditResult&) { completion_fired = true; });

    access.call_handle_save();
    UpdateQueue::instance().drain();

    REQUIRE(completion_fired);
    // Re-save always re-syncs so Moonraker recovers lost state.
    REQUIRE(api.spoolman_mock().get_mock_active_spool_id() == 42);
}

TEST_CASE_METHOD(LVGLTestFixture, "handle_save does NOT crash when no API available",
                 "[ams_edit_modal][spoolman][active_spool]") {
    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);
    access.init_subjects();

    SlotInfo original;
    original.spoolman_id = 0;

    SlotInfo working;
    working.spoolman_id = 42;

    access.set_original_info(original);
    access.set_working_info(working);
    access.set_api(nullptr);
    access.set_slot_index(-2);

    bool completion_fired = false;
    access.set_completion_callback([&](const AmsEditModal::EditResult& result) {
        completion_fired = true;
        REQUIRE(result.saved);
    });

    access.call_handle_save();
    UpdateQueue::instance().drain();
    REQUIRE(completion_fired);
}

// ============================================================================
// Tests: #1071 create-gate — should_create_new_spool() must block a new spool
// on an unedited open+save (the auto-defaulted "Generic") while still allowing
// one after a genuine user filament edit. Tested as a pure predicate; the
// surrounding is_spoolman_available() gate + Spoolman create chain are not
// unit-harnessable here, but this predicate is the sole decision they consume.
// ============================================================================

TEST_CASE("AmsEditModal::should_create_new_spool gates new-spool creation on a real edit (#1071)",
          "[ams_edit_modal][spoolman][1071]") {
    // Unlinked slot whose fields look complete only because update_vendor_dropdown
    // auto-defaulted brand="Generic" — exactly the unedited open the bug created a
    // phantom spool from.
    SlotInfo working;
    working.spoolman_id = 0;
    working.brand = "Generic";
    working.material = "PLA";
    working.color_rgb = 0xFF0000;
    REQUIRE(helix::SpoolmanSlotSaver::is_filament_complete(working));

    // No user edit -> NO new spool (the #1071 Symptom C fix).
    CHECK_FALSE(AmsEditModalTestAccess::should_create_new_spool(working, /*edited=*/false));

    // Genuine user edit on the same complete fields -> create allowed (the gate
    // must not block a legitimate manual entry).
    CHECK(AmsEditModalTestAccess::should_create_new_spool(working, /*edited=*/true));

    // A slot already linked to Spoolman never takes the create path, edit or not
    // (it updates the linked spool instead).
    SlotInfo linked = working;
    linked.spoolman_id = 99;
    CHECK_FALSE(AmsEditModalTestAccess::should_create_new_spool(linked, /*edited=*/true));

    // Incomplete metadata never creates, even after an edit.
    SlotInfo incomplete;
    incomplete.spoolman_id = 0;
    incomplete.material = "PLA"; // no brand, default color
    REQUIRE_FALSE(helix::SpoolmanSlotSaver::is_filament_complete(incomplete));
    CHECK_FALSE(AmsEditModalTestAccess::should_create_new_spool(incomplete, /*edited=*/true));
}

TEST_CASE("AmsEditModal::is_material_identity_change flags different-spool edits (#1071)",
          "[ams_edit_modal][spoolman][1071]") {
    SlotInfo original;
    original.material = "PLA";
    original.color_rgb = 0xFF0000;

    // Same material + same color: a plain re-save, NOT an identity change.
    SlotInfo same = original;
    CHECK_FALSE(AmsEditModalTestAccess::is_material_identity_change(original, same));

    // Tiny color tweak within the match tolerance: still the same spool.
    SlotInfo nudged = original;
    nudged.color_rgb = 0xFE0101;
    CHECK_FALSE(AmsEditModalTestAccess::is_material_identity_change(original, nudged));

    // Material comparison is case-insensitive: not a change.
    SlotInfo recased = original;
    recased.material = "pla";
    CHECK_FALSE(AmsEditModalTestAccess::is_material_identity_change(original, recased));

    // Different material: identity change (confirm before overwriting the link).
    SlotInfo diff_mat = original;
    diff_mat.material = "PETG";
    CHECK(AmsEditModalTestAccess::is_material_identity_change(original, diff_mat));

    // Far-apart color (red -> blue): identity change.
    SlotInfo diff_color = original;
    diff_color.color_rgb = 0x0000FF;
    CHECK(AmsEditModalTestAccess::is_material_identity_change(original, diff_color));
}

// ============================================================================
// Tests: #1071 initial-view routing — show_for_slot(..., open_on_picker) must
// open directly on the Spoolman picker; the default must open on the form.
// Uses the full-UI fixture so Modal::show() finds the registered
// ams_edit_modal XML component and the view-mode subject reflects the choice.
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot opens on the Spoolman picker when requested (#1071)",
                 "[ams_edit_modal][spoolman][ui_integration][1071]") {
    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);

    SlotInfo info;
    info.slot_index = 0;

    REQUIRE(modal.show_for_slot(test_screen(), 0, info, api(), /*open_on_picker=*/true));
    process_lvgl(10);

    // Picker view is selected synchronously by switch_to_picker().
    CHECK(access.get_view_mode() == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "show_for_slot opens on the form view by default (#1071)",
                 "[ams_edit_modal][spoolman][ui_integration][1071]") {
    AmsEditModal modal;
    AmsEditModalTestAccess access(modal);

    SlotInfo info;
    info.slot_index = 0;

    REQUIRE(modal.show_for_slot(test_screen(), 0, info, api()));
    process_lvgl(10);

    // Default path (open_on_picker=false) stays on the form view.
    CHECK(access.get_view_mode() == 0);
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

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

    void init_subjects() {
        modal_.init_subjects();
    }
    void call_handle_save() {
        modal_.handle_save();
    }

    void set_completion_callback(AmsEditModal::CompletionCallback cb) {
        modal_.completion_callback_ = std::move(cb);
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

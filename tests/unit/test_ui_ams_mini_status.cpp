// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_ams_mini_status.cpp
 * @brief Tests for the ams_mini_status home dashboard widget (wide spool view)
 */

#include "ui_ams_mini_status.h"
#include "config.h"
#include "panel_widget_registry.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "ams registry: scalable to 4x wide",
                 "[ui][ams_mini][registry]") {
    const PanelWidgetDef* def = find_widget_def("ams");
    REQUIRE(def != nullptr);
    REQUIRE(def->max_colspan == 4);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: set_width accepts colspan", "[ui][ams_mini]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 40);
    REQUIRE(w != nullptr);
    ui_ams_mini_status_set_width(w, 260, 2); // new 3-arg signature
    SUCCEED("compiles and runs with colspan arg");
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: colspan>=2 selects spool mode",
                 "[ui][ams_mini][mode]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    // Flush any initial AmsState auto-sync queued at create time (a prior test in
    // the shard may have left AmsState with slots). The widget auto-binds to
    // AmsState and sync_from_ams_state() would otherwise clobber slot_count below.
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 2);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 70, true, "PLA", 70);
    ui_ams_mini_status_set_slot_full(w, 1, 0x00FF00, 40, true, "PETG", 40);

    ui_ams_mini_status_set_width(w, 130, 1); // 1x -> bar mode, no spools container
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "ams_spools_container") == nullptr);

    ui_ams_mini_status_set_width(w, 260, 2); // 2x -> spool mode
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: cell has spool + material + pct",
                 "[ui][ams_mini][spool]") {
    helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain(); // flush stray auto-sync
    ui_ams_mini_status_set_slot_count(w, 1);
    ui_ams_mini_status_set_slot_full(w, 0, 0xFF0000, 73, true, "PLA", 73);
    ui_ams_mini_status_set_width(w, 260, 2);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* mat = UITest::find_by_name(w, "spool_material_0");
    lv_obj_t* pct = UITest::find_by_name(w, "spool_pct_0");
    REQUIRE(mat != nullptr);
    REQUIRE(pct != nullptr);
    REQUIRE(std::string(lv_label_get_text(mat)) == "PLA");
    REQUIRE(std::string(lv_label_get_text(pct)) == "73%");
    REQUIRE(UITest::find_by_name(w, "spool_cell_0") != nullptr);
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: empty slot shows -- and no pct",
                 "[ui][ams_mini][spool]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 1);
    ui_ams_mini_status_set_slot_full(w, 0, 0x808080, 0, false, "", -1);
    ui_ams_mini_status_set_width(w, 260, 2);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(std::string(lv_label_get_text(UITest::find_by_name(w, "spool_material_0"))) == "--");
    REQUIRE(std::string(lv_label_get_text(UITest::find_by_name(w, "spool_pct_0"))) == "");
    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: all slots present incl multi-unit",
                 "[ui][ams_mini][spool]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    ui_ams_mini_status_set_slot_count(w, 12); // > MAX_VISIBLE(8): uncapped vector
    for (int i = 0; i < 12; ++i)
        ui_ams_mini_status_set_slot_full(w, i, 0xFF0000, 50, true, "PLA", 50);
    ui_ams_mini_status_set_width(w, 520, 4);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE(lv_obj_get_child_count(spools) == 12); // single row, all slots; overflow scrolls
    REQUIRE(UITest::find_by_name(w, "spool_cell_11") != nullptr);
    lv_obj_delete(w);
}

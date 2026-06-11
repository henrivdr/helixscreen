// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_ams_mini_status.cpp
 * @brief Tests for the ams_mini_status home dashboard widget (wide spool view)
 */

#include "ui_ams_mini_status.h"
#include "panel_widget_registry.h"

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

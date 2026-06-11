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

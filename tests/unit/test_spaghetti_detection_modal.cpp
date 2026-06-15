// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

// LVGLUITestFixture registers ALL XML components (via
// helix::register_xml_components()), including spaghetti_detection_modal.xml,
// so the modal can be created from XML inside the test.
#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture,
                 "SpaghettiDetectionModal shows message + invokes callbacks",
                 "[detection][modal][.ui_integration]") {
    SpaghettiDetectionModal modal;
    int resumed = 0, aborted = 0, tuned = 0;
    modal.set_on_resume([&] { ++resumed; });
    modal.set_on_abort([&] { ++aborted; });
    modal.set_on_tune([&] { ++tuned; });
    modal.set_detection("detected noodle", nullptr);

    // Resume path.
    REQUIRE(modal.show(test_screen()));
    REQUIRE(modal.is_visible());
    modal.invoke_resume_for_test();
    REQUIRE(resumed == 1);
    REQUIRE(aborted == 0);
    REQUIRE_FALSE(modal.is_visible()); // Resume hides the modal

    // Abort path.
    REQUIRE(modal.show(test_screen()));
    REQUIRE(modal.is_visible());
    modal.invoke_abort_for_test();
    REQUIRE(aborted == 1);
    REQUIRE(resumed == 1);
    REQUIRE_FALSE(modal.is_visible()); // Abort hides the modal

    // Tune does NOT hide; it only invokes the callback.
    REQUIRE(modal.show(test_screen()));
    modal.invoke_tune_for_test();
    REQUIRE(tuned == 1);
    REQUIRE(modal.is_visible());
    modal.hide();

    process_lvgl(50);
}

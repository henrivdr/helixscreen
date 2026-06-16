// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

// LVGLUITestFixture registers ALL XML components (via
// helix::register_xml_components()), including spaghetti_detection_modal.xml,
// so the modal can be created from XML inside the test.
#include "../lvgl_ui_test_fixture.h"

#include "../catch_amalgamated.hpp"

// NOTE: SpaghettiDetectionModal self-deletes via its on_hide() override
// (async_call(delete this)) — matching its production usage, where the modal is
// always heap-allocated and dropped after show(). These tests therefore heap-
// allocate each modal and let process_lvgl() drain the queued self-delete. A
// stack-allocated modal would double-free here, so do NOT switch these to stack.

TEST_CASE_METHOD(LVGLUITestFixture,
                 "SpaghettiDetectionModal shows message + invokes callbacks",
                 "[detection][modal][.ui_integration]") {
    int resumed = 0, aborted = 0, tuned = 0;

    // Resume path: heap modal self-deletes on hide.
    {
        auto* modal = new SpaghettiDetectionModal();
        modal->set_on_resume([&] { ++resumed; });
        modal->set_on_abort([&] { ++aborted; });
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(modal->show(test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_resume_for_test(); // invokes callback then hide() -> self-delete
        REQUIRE(resumed == 1);
        REQUIRE(aborted == 0);
        process_lvgl(50); // drain the async self-delete; do not touch modal after this
    }

    // Abort path.
    {
        auto* modal = new SpaghettiDetectionModal();
        modal->set_on_abort([&] { ++aborted; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(modal->show(test_screen()));
        REQUIRE(modal->is_visible());
        modal->invoke_abort_for_test();
        REQUIRE(aborted == 1);
        REQUIRE(resumed == 1);
        process_lvgl(50);
    }

    // Tune does NOT hide; it only invokes the callback. The modal stays alive
    // until we hide() it explicitly.
    {
        auto* modal = new SpaghettiDetectionModal();
        modal->set_on_tune([&] { ++tuned; });
        modal->set_detection("detected noodle", nullptr);
        REQUIRE(modal->show(test_screen()));
        modal->invoke_tune_for_test();
        REQUIRE(tuned == 1);
        REQUIRE(modal->is_visible()); // still visible: Tune does not hide
        modal->hide();                // triggers on_hide() -> self-delete
        process_lvgl(50);
    }
}

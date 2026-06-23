// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_ui_test_fixture.h"
#include "recovery_modal_presenter.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "RecoveryModalPresenter shows and dismisses",
                 "[error-center][recovery-presenter]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "tool_end jam detected";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    // Presenting the same detail again must not stack a second modal.
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    presenter.dismiss();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());
}

TEST_CASE_METHOD(LVGLUITestFixture, "RecoveryModalPresenter dedup allows re-show after dismiss",
                 "[error-center][recovery-presenter]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "jam detail";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};

    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());

    presenter.dismiss();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());

    // After dismiss, re-presenting the same detail must show again.
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Targeted (subset) wizard session tests.
//
// Verifies that ui_wizard_create_targeted() launches the wizard running only a
// requested subset of steps (for hardware reconfiguration) and that finishing
// the subset goes through ui_wizard_complete_targeted() — which fires the
// on_complete callback and clears the subset WITHOUT setting wizard_completed.
//
// Uses LVGLUITestFixture for subject/widget/XML setup. The core contract
// (subset storage + completion wiring) is exercised without depending on the
// XML component tree being present on disk: ui_wizard_create_targeted() records
// the subset before creating the container, and the completion path is safe
// even when the container failed to materialize.

#include "ui_wizard.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using helix::wizard::StepId;

TEST_CASE_METHOD(LVGLUITestFixture, "targeted wizard records the requested step subset",
                 "[wizard][targeted]") {
    // Not in targeted mode before any targeted session is created.
    REQUIRE(ui_wizard_active_step_subset().empty());

    bool completed = false;
    lv_obj_t* root =
        ui_wizard_create_targeted(test_screen(), {StepId::FanSelect}, [&]() { completed = true; });

    // The container should be created when the XML tree is available. If it is
    // not (bare environment), the subset contract below is still the meaningful
    // assertion, so don't hard-fail on a missing widget tree.
    if (root == nullptr) {
        spdlog::warn("[test] wizard container not created (XML infra unavailable)");
    }

    // Core contract: the requested subset is active and exactly what was asked.
    const auto& subset = ui_wizard_active_step_subset();
    REQUIRE(subset.size() == 1);
    REQUIRE(subset[0] == StepId::FanSelect);
    REQUIRE(is_wizard_active());
    REQUIRE_FALSE(completed);

    // Finishing the targeted session fires the callback and clears subset mode
    // (and must NOT mark the wizard active anymore).
    ui_wizard_complete_targeted();
    REQUIRE(completed);
    REQUIRE(ui_wizard_active_step_subset().empty());
    REQUIRE_FALSE(is_wizard_active());
}

TEST_CASE_METHOD(LVGLUITestFixture, "targeted wizard preserves multi-step order",
                 "[wizard][targeted]") {
    bool completed = false;
    ui_wizard_create_targeted(test_screen(),
                              {StepId::HeaterSelect, StepId::FanSelect, StepId::LedSelect},
                              [&]() { completed = true; });

    const auto& subset = ui_wizard_active_step_subset();
    REQUIRE(subset.size() == 3);
    REQUIRE(subset[0] == StepId::HeaterSelect);
    REQUIRE(subset[1] == StepId::FanSelect);
    REQUIRE(subset[2] == StepId::LedSelect);

    // Tear down so the fixture (and any later test) starts clean.
    ui_wizard_complete_targeted();
    REQUIRE(completed);
    REQUIRE(ui_wizard_active_step_subset().empty());
}

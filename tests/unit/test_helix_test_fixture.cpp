// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture::reset_all resets SystemSettingsManager language",
                 "[test-fixture][isolation]") {
    // Ctor reset already ran — default should be visible.
    REQUIRE(SystemSettingsManager::instance().get_language() == "en");

    // Mutation observable.
    SystemSettingsManager::instance().set_language("fr");
    REQUIRE(SystemSettingsManager::instance().get_language() == "fr");

    // Manual reset restores default — proves reset_all() itself resets, not just ctor.
    HelixTestFixture::reset_all();
    REQUIRE(SystemSettingsManager::instance().get_language() == "en");
}

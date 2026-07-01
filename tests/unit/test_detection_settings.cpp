// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager detection_enabled round-trip",
                 "[detection][settings]") {
    Config::get_instance();
    SettingsManager::instance().init_subjects();

    SECTION("default is true") {
        REQUIRE(SettingsManager::instance().get_detection_enabled() == true);
    }

    SECTION("set false then get") {
        SettingsManager::instance().set_detection_enabled(false);
        REQUIRE(SettingsManager::instance().get_detection_enabled() == false);
    }

    SECTION("set true then get") {
        SettingsManager::instance().set_detection_enabled(false);
        SettingsManager::instance().set_detection_enabled(true);
        REQUIRE(SettingsManager::instance().get_detection_enabled() == true);
    }

    SettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager detection_policy_u1 round-trip",
                 "[detection][settings]") {
    Config::get_instance();
    SettingsManager::instance().init_subjects();

    SECTION("default is 2 (DeferToSource)") {
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 2);
    }

    SECTION("set to 0 (Off)") {
        SettingsManager::instance().set_detection_policy_u1(0);
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 0);
    }

    SECTION("set to 1 (NotifyOnly)") {
        SettingsManager::instance().set_detection_policy_u1(1);
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 1);
    }

    SECTION("set to 2 (DeferToSource)") {
        SettingsManager::instance().set_detection_policy_u1(0);
        SettingsManager::instance().set_detection_policy_u1(2);
        REQUIRE(SettingsManager::instance().get_detection_policy_u1() == 2);
    }

    SettingsManager::instance().deinit_subjects();
}

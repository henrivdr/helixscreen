// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "display_settings_manager.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// DisplaySettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager default values after init",
                 "[display_settings]") {
    // Reset config to defaults so prior tests' set_*() calls don't pollute
    Config::get_instance()->set<int>("/display/sleep_sec", 1200);
    Config::get_instance()->set<int>("/display/dim_sec", 600);
    Config::get_instance()->set<int>("/display/brightness", 80);
    Config::get_instance()->set<bool>("/display/animations_enabled", true);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("dark_mode defaults to true (dark)") {
        REQUIRE(DisplaySettingsManager::instance().get_dark_mode() == true);
    }

    SECTION("dark_mode_available defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().is_dark_mode_available() == true);
    }

    SECTION("display_dim defaults to 600 seconds") {
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 600);
    }

    SECTION("display_sleep defaults to 1200 seconds") {
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 1200);
    }

    SECTION("brightness defaults to 80") {
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 80);
    }

    SECTION("sleep_while_printing defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == true);
    }

    SECTION("animations_enabled defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == true);
    }

    SECTION("bed_mesh_render_mode defaults to 0 (Auto)") {
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 0);
    }

    SECTION("gcode_render_mode defaults to 0 (Auto)") {
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 0);
    }

    SECTION("time_format defaults to HOUR_12") {
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12);
    }

    SECTION("bed_mesh_show_zero_plane defaults to true") {
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_show_zero_plane() == true);
    }

    SECTION("printer_image defaults to empty string") {
        REQUIRE(DisplaySettingsManager::instance().get_printer_image().empty());
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager set/get round trips",
                 "[display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("brightness set/get") {
        DisplaySettingsManager::instance().set_brightness(75);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 75);

        DisplaySettingsManager::instance().set_brightness(10);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(100);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 100);
    }

    SECTION("brightness clamping - below 10 clamps to 10") {
        DisplaySettingsManager::instance().set_brightness(5);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(0);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);

        DisplaySettingsManager::instance().set_brightness(-10);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 10);
    }

    SECTION("brightness clamping - above 100 clamps to 100") {
        DisplaySettingsManager::instance().set_brightness(200);
        REQUIRE(DisplaySettingsManager::instance().get_brightness() == 100);
    }

    SECTION("animations_enabled set/get") {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == false);

        DisplaySettingsManager::instance().set_animations_enabled(true);
        REQUIRE(DisplaySettingsManager::instance().get_animations_enabled() == true);
    }

    SECTION("keep_navbar_visible set/get") {
        DisplaySettingsManager::instance().set_keep_navbar_visible(true);
        REQUIRE(DisplaySettingsManager::instance().get_keep_navbar_visible() == true);

        DisplaySettingsManager::instance().set_keep_navbar_visible(false);
        REQUIRE(DisplaySettingsManager::instance().get_keep_navbar_visible() == false);
    }

    SECTION("display_dim set/get") {
        DisplaySettingsManager::instance().set_display_dim_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 60);

        DisplaySettingsManager::instance().set_display_dim_sec(0);
        REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 0);
    }

    SECTION("display_sleep set/get") {
        DisplaySettingsManager::instance().set_display_sleep_sec(600);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);

        DisplaySettingsManager::instance().set_display_sleep_sec(0);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 0);
    }

    SECTION("time_format set/get") {
        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_24);
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_24);

        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_12);
        REQUIRE(DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12);
    }

    SECTION("sleep_while_printing set/get") {
        DisplaySettingsManager::instance().set_sleep_while_printing(false);
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == false);

        DisplaySettingsManager::instance().set_sleep_while_printing(true);
        REQUIRE(DisplaySettingsManager::instance().get_sleep_while_printing() == true);
    }

    SECTION("bed_mesh_render_mode set/get") {
        DisplaySettingsManager::instance().set_bed_mesh_render_mode(1);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 1);

        DisplaySettingsManager::instance().set_bed_mesh_render_mode(2);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 2);

        DisplaySettingsManager::instance().set_bed_mesh_render_mode(0);
        REQUIRE(DisplaySettingsManager::instance().get_bed_mesh_render_mode() == 0);
    }

    SECTION("gcode_render_mode set/get") {
        DisplaySettingsManager::instance().set_gcode_render_mode(2);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 2);

        DisplaySettingsManager::instance().set_gcode_render_mode(1);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 1);

        DisplaySettingsManager::instance().set_gcode_render_mode(0);
        REQUIRE(DisplaySettingsManager::instance().get_gcode_render_mode() == 0);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager dim seconds to index conversion",
                 "[display_settings]") {
    // dim_seconds_to_index: 0=Never, 30=30sec, 60=1min, 120=2min, 300=5min, 600=10min
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(0) == 0);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(30) == 1);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(60) == 2);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(120) == 3);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(300) == 4);
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(600) == 5);

    // Unknown value defaults to index 5 (10 minutes)
    REQUIRE(DisplaySettingsManager::dim_seconds_to_index(999) == 5);

    // index_to_dim_seconds round-trip
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(0) == 0);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(1) == 30);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(2) == 60);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(3) == 120);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(4) == 300);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(5) == 600);

    // Out of range defaults to 600 (10 minutes)
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(-1) == 600);
    REQUIRE(DisplaySettingsManager::index_to_dim_seconds(99) == 600);
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager sleep seconds to index conversion",
                 "[display_settings]") {
    // sleep_seconds_to_index: 0=Never, 60=1min, 300=5min, 600=10min, 1200=20min, 1800=30min
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(0) == 0);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(60) == 1);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(300) == 2);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(600) == 3);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(1200) == 4);
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(1800) == 5);

    // Unknown value defaults to index 4 (20 minutes)
    REQUIRE(DisplaySettingsManager::sleep_seconds_to_index(999) == 4);

    // index_to_sleep_seconds round-trip
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(0) == 0);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(1) == 60);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(2) == 300);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(3) == 600);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(4) == 1200);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(5) == 1800);

    // Out of range defaults to 1200 (20 minutes)
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(-1) == 1200);
    REQUIRE(DisplaySettingsManager::index_to_sleep_seconds(99) == 1200);
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager subject values match getters",
                 "[display_settings]") {
    Config::get_instance();
    DisplaySettingsManager::instance().init_subjects();

    SECTION("brightness subject reflects setter") {
        DisplaySettingsManager::instance().set_brightness(55);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_brightness()) == 55);
    }

    SECTION("animations_enabled subject reflects setter") {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_animations_enabled()) == 0);

        DisplaySettingsManager::instance().set_animations_enabled(true);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_animations_enabled()) == 1);
    }

    SECTION("keep_navbar_visible subject reflects setter") {
        DisplaySettingsManager::instance().set_keep_navbar_visible(true);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_keep_navbar_visible()) == 1);

        DisplaySettingsManager::instance().set_keep_navbar_visible(false);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_keep_navbar_visible()) == 0);
    }

    SECTION("time_format subject reflects setter") {
        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_24);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_time_format()) == 1);

        DisplaySettingsManager::instance().set_time_format(TimeFormat::HOUR_12);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_time_format()) == 0);
    }

    SECTION("display_dim subject reflects setter") {
        DisplaySettingsManager::instance().set_display_dim_sec(120);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_display_dim()) ==
                120);
    }

    SECTION("display_sleep subject reflects setter") {
        DisplaySettingsManager::instance().set_display_sleep_sec(600);
        REQUIRE(lv_subject_get_int(DisplaySettingsManager::instance().subject_display_sleep()) ==
                600);
    }

    SECTION("bed_mesh_render_mode subject reflects setter") {
        DisplaySettingsManager::instance().set_bed_mesh_render_mode(2);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_bed_mesh_render_mode()) == 2);
    }

    SECTION("gcode_render_mode subject reflects setter") {
        DisplaySettingsManager::instance().set_gcode_render_mode(1);
        REQUIRE(lv_subject_get_int(
                    DisplaySettingsManager::instance().subject_gcode_render_mode()) == 1);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "DisplaySettingsManager options strings", "[display_settings]") {
    SECTION("dim options") {
        const char* options = DisplaySettingsManager::get_display_dim_options();
        REQUIRE(options != nullptr);
        REQUIRE(std::string(options).find("Never") != std::string::npos);
        REQUIRE(std::string(options).find("5 minutes") != std::string::npos);
    }

    SECTION("sleep options") {
        const char* options = DisplaySettingsManager::get_display_sleep_options();
        REQUIRE(options != nullptr);
        REQUIRE(std::string(options).find("Never") != std::string::npos);
        REQUIRE(std::string(options).find("30 minutes") != std::string::npos);
    }

    SECTION("bed mesh render mode options") {
        const char* options = DisplaySettingsManager::get_bed_mesh_render_mode_options();
        REQUIRE(options != nullptr);
        REQUIRE(std::string(options) == "Auto\n3D View\n2D Heatmap");
    }

    SECTION("gcode render mode options") {
        const char* options = DisplaySettingsManager::get_gcode_render_mode_options();
        REQUIRE(options != nullptr);
        REQUIRE(std::string(options) == "Auto\n3D View\n2D Layers\nThumbnail Only");
    }

    SECTION("time format options") {
        const char* options = DisplaySettingsManager::get_time_format_options();
        REQUIRE(options != nullptr);
        REQUIRE(std::string(options) == "12 Hour\n24 Hour");
    }
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cli_args.cpp
 * @brief Unit tests for CLI argument struct helpers
 *
 * Tests the OverlayFlags and CliArgs struct inline methods, and
 * parse_cli_args() for flags that do not require graphics or printer state.
 */

#include "cli_args.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// OverlayFlags Tests
// ============================================================================

TEST_CASE("OverlayFlags: needs_moonraker", "[cli_args]") {
    SECTION("default flags don't need moonraker") {
        OverlayFlags flags;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("motion overlay needs moonraker") {
        OverlayFlags flags;
        flags.motion = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("nozzle_temp overlay needs moonraker") {
        OverlayFlags flags;
        flags.nozzle_temp = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("bed_temp overlay needs moonraker") {
        OverlayFlags flags;
        flags.bed_temp = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("fan overlay needs moonraker") {
        OverlayFlags flags;
        flags.fan = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("print_status overlay needs moonraker") {
        OverlayFlags flags;
        flags.print_status = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("bed_mesh overlay needs moonraker") {
        OverlayFlags flags;
        flags.bed_mesh = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("zoffset overlay needs moonraker") {
        OverlayFlags flags;
        flags.zoffset = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("pid overlay needs moonraker") {
        OverlayFlags flags;
        flags.pid = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("screws_tilt overlay needs moonraker") {
        OverlayFlags flags;
        flags.screws_tilt = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("input_shaper overlay needs moonraker") {
        OverlayFlags flags;
        flags.input_shaper = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("file_detail overlay needs moonraker") {
        OverlayFlags flags;
        flags.file_detail = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("history_dashboard overlay needs moonraker") {
        OverlayFlags flags;
        flags.history_dashboard = true;
        REQUIRE(flags.needs_moonraker());
    }

    SECTION("spoolman overlay needs moonraker") {
        OverlayFlags flags;
        flags.spoolman = true;
        REQUIRE(flags.needs_moonraker());
    }

    // Overlays that do NOT need moonraker
    SECTION("keypad overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.keypad = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("keyboard overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.keyboard = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("glyphs overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.glyphs = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("theme overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.theme = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("theme_edit overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.theme_edit = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("test_panel overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.test_panel = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("step_test overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.step_test = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("gcode_test overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.gcode_test = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("gradient_test overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.gradient_test = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("print_select_list does NOT need moonraker") {
        OverlayFlags flags;
        flags.print_select_list = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("ams overlay does NOT need moonraker") {
        OverlayFlags flags;
        flags.ams = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("wizard_ams_identify does NOT need moonraker") {
        OverlayFlags flags;
        flags.wizard_ams_identify = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("display_settings does NOT need moonraker") {
        OverlayFlags flags;
        flags.display_settings = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("sensor_settings does NOT need moonraker") {
        OverlayFlags flags;
        flags.sensor_settings = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("touch_calibration does NOT need moonraker") {
        OverlayFlags flags;
        flags.touch_calibration = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("hardware_health does NOT need moonraker") {
        OverlayFlags flags;
        flags.hardware_health = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("network_settings does NOT need moonraker") {
        OverlayFlags flags;
        flags.network_settings = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("macros does NOT need moonraker") {
        OverlayFlags flags;
        flags.macros = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }

    SECTION("print_tune does NOT need moonraker") {
        OverlayFlags flags;
        flags.print_tune = true;
        REQUIRE_FALSE(flags.needs_moonraker());
    }
}

// ============================================================================
// CliArgs Tests
// ============================================================================

TEST_CASE("parse_cli_args: --detect-printer with host/port", "[cli_args][detect]") {
    const char* argv[] = {"helix-screen", "--detect-printer", "--host",
                          "127.0.0.1",    "--port",           "7125"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(6, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.detect_printer);
    REQUIRE(args.detect_host == "127.0.0.1");
    REQUIRE(args.detect_port == 7125);
}

TEST_CASE("parse_cli_args: --detect-printer defaults", "[cli_args][detect]") {
    const char* argv[] = {"helix-screen", "--detect-printer"};
    CliArgs args;
    int w = 0, h = 0;
    REQUIRE(parse_cli_args(2, const_cast<char**>(argv), args, w, h));
    REQUIRE(args.detect_printer);
    REQUIRE(args.detect_host == "127.0.0.1");
    REQUIRE(args.detect_port == 7125);
}

TEST_CASE("CliArgs: default values", "[cli_args]") {
    CliArgs args;

    SECTION("screen settings default to auto") {
        REQUIRE(args.screen_size == ScreenSize::MEDIUM);
        REQUIRE(args.dpi == -1);
        REQUIRE(args.display_num == -1);
        REQUIRE(args.x_pos == -1);
        REQUIRE(args.y_pos == -1);
    }

    SECTION("panel navigation defaults to auto") {
        REQUIRE(args.initial_panel == -1);
        REQUIRE_FALSE(args.panel_requested);
    }

    SECTION("wizard defaults off") {
        REQUIRE_FALSE(args.force_wizard);
        REQUIRE(args.wizard_step == -1);
    }

    SECTION("automation defaults off") {
        REQUIRE_FALSE(args.screenshot_enabled);
        REQUIRE(args.screenshot_delay_sec == 2);
        REQUIRE(args.timeout_sec == 0);
    }

    SECTION("theme defaults to not set") {
        REQUIRE(args.dark_mode_cli == -1);
    }

    SECTION("logging defaults to warning level") {
        REQUIRE(args.verbosity == 0);
    }

    SECTION("memory profiling defaults off") {
        REQUIRE_FALSE(args.memory_report);
        REQUIRE_FALSE(args.show_memory);
    }

    SECTION("moonraker URL default empty") {
        REQUIRE(args.moonraker_url.empty());
    }

    SECTION("layout default empty") {
        REQUIRE(args.layout.empty());
    }
}

TEST_CASE("CliArgs: needs_moonraker_data", "[cli_args]") {
    SECTION("default args don't need moonraker") {
        CliArgs args;
        REQUIRE_FALSE(args.needs_moonraker_data());
    }

    SECTION("specific panel request needs moonraker") {
        CliArgs args;
        args.initial_panel = 1; // Any panel ID >= 0
        REQUIRE(args.needs_moonraker_data());
    }

    SECTION("overlay needing moonraker propagates") {
        CliArgs args;
        args.overlays.bed_mesh = true;
        REQUIRE(args.needs_moonraker_data());
    }

    SECTION("overlay NOT needing moonraker doesn't trigger") {
        CliArgs args;
        args.overlays.theme = true;
        REQUIRE_FALSE(args.needs_moonraker_data());
    }
}

// ============================================================================
// ScreenSize Enum Tests
// ============================================================================

TEST_CASE("ScreenSize enum values", "[cli_args]") {
    SECTION("All enum values are distinct") {
        REQUIRE(ScreenSize::TINY != ScreenSize::SMALL);
        REQUIRE(ScreenSize::TINY != ScreenSize::MEDIUM);
        REQUIRE(ScreenSize::TINY != ScreenSize::LARGE);
        REQUIRE(ScreenSize::TINY != ScreenSize::XLARGE);
        REQUIRE(ScreenSize::SMALL != ScreenSize::MEDIUM);
        REQUIRE(ScreenSize::SMALL != ScreenSize::LARGE);
        REQUIRE(ScreenSize::SMALL != ScreenSize::XLARGE);
        REQUIRE(ScreenSize::MEDIUM != ScreenSize::LARGE);
        REQUIRE(ScreenSize::MEDIUM != ScreenSize::XLARGE);
        REQUIRE(ScreenSize::LARGE != ScreenSize::XLARGE);
    }

    SECTION("ScreenSize ordering matches expected breakpoint order") {
        // Verify enum values are ordered TINY < SMALL < MEDIUM < LARGE < XLARGE
        REQUIRE(static_cast<int>(ScreenSize::TINY) < static_cast<int>(ScreenSize::SMALL));
        REQUIRE(static_cast<int>(ScreenSize::SMALL) < static_cast<int>(ScreenSize::MEDIUM));
        REQUIRE(static_cast<int>(ScreenSize::MEDIUM) < static_cast<int>(ScreenSize::LARGE));
        REQUIRE(static_cast<int>(ScreenSize::LARGE) < static_cast<int>(ScreenSize::XLARGE));
    }

    SECTION("Default CliArgs screen_size is MEDIUM") {
        CliArgs args;
        REQUIRE(args.screen_size == ScreenSize::MEDIUM);
    }
}

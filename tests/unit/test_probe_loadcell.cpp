// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_loadcell.cpp
 * @brief Unit tests for load-cell probe detection + calibration command strings.
 *
 * Mirrors the eddy-current probe pattern:
 *  - Detection: ProbeSensorManager::discover() recognizes "load_cell",
 *    "load_cell <name>", and "load_cell_probe" Klipper objects and tags them
 *    ProbeSensorType::LOADCELL.
 *  - Calibration commands: helix::sensors::build_loadcell_command() produces the
 *    LOAD_CELL_TARE / LOAD_CELL_CALIBRATE / LOAD_CELL_DIAGNOSTIC gcode, appending
 *    LOAD_CELL=<name> only when the sensor carries a distinct name.
 *
 * These would FAIL if load-cell support were removed (discover would not tag
 * LOADCELL; the command builder would not exist / not emit the right gcode).
 */

#include "../ui_test_utils.h"
#include "probe_sensor_manager.h"
#include "probe_sensor_types.h"

#include <spdlog/spdlog.h>

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::sensors;

// ============================================================================
// Test Access (mirror of test_probe_sensor_manager.cpp)
// ============================================================================

namespace helix::sensors {
class ProbeLoadCellTestAccess {
  public:
    static void reset(ProbeSensorManager& obj) {
        std::lock_guard<std::recursive_mutex> lock(obj.mutex_);
        obj.sensors_.clear();
        obj.states_.clear();
        obj.sync_mode_ = true;
        if (obj.subjects_initialized_) {
            lv_subject_set_int(&obj.sensor_count_, 0);
        }
    }
};
} // namespace helix::sensors

namespace {

class LoadCellTestFixture {
  public:
    LoadCellTestFixture() {
        lv_init_safe();
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(
                display_, [](lv_display_t* disp, const lv_area_t* /*a*/, uint8_t* /*p*/) {
                    lv_display_flush_ready(disp);
                });
            display_created_ = true;
        }
        mgr().init_subjects();
        ProbeLoadCellTestAccess::reset(mgr());
    }
    ~LoadCellTestFixture() {
        ProbeLoadCellTestAccess::reset(mgr());
    }

  protected:
    ProbeSensorManager& mgr() {
        return ProbeSensorManager::instance();
    }

  private:
    static lv_display_t* display_;
    static bool display_created_;
};

lv_display_t* LoadCellTestFixture::display_ = nullptr;
bool LoadCellTestFixture::display_created_ = false;

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE_METHOD(LoadCellTestFixture, "ProbeSensorManager - load cell detection",
                 "[probe][loadcell][discovery]") {
    SECTION("Detects bare load_cell object") {
        mgr().discover({"load_cell"});
        REQUIRE(mgr().has_sensors());
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "load_cell");
        REQUIRE(configs[0].sensor_name == "load_cell");
        REQUIRE(configs[0].type == ProbeSensorType::LOADCELL);
    }

    SECTION("Detects named load_cell object") {
        mgr().discover({"load_cell my_cell"});
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "load_cell my_cell");
        REQUIRE(configs[0].sensor_name == "my_cell");
        REQUIRE(configs[0].type == ProbeSensorType::LOADCELL);
    }

    SECTION("Detects load_cell_probe object") {
        mgr().discover({"load_cell_probe"});
        REQUIRE(mgr().sensor_count() == 1);

        auto configs = mgr().get_sensors();
        REQUIRE(configs[0].klipper_name == "load_cell_probe");
        REQUIRE(configs[0].type == ProbeSensorType::LOADCELL);
    }

    SECTION("Non-load-cell objects are not tagged LOADCELL") {
        mgr().discover({"probe_eddy_current btt"});
        REQUIRE(mgr().sensor_count() == 1);
        REQUIRE(mgr().get_sensors()[0].type == ProbeSensorType::EDDY_CURRENT);
    }
}

// ============================================================================
// Type string round-trip
// ============================================================================

TEST_CASE("LoadCell probe type string conversions", "[probe][loadcell][types]") {
    REQUIRE(probe_type_to_string(ProbeSensorType::LOADCELL) == "loadcell");
    REQUIRE(probe_type_from_string("loadcell") == ProbeSensorType::LOADCELL);
    REQUIRE(probe_type_to_display_string(ProbeSensorType::LOADCELL) == "Load Cell");
}

// ============================================================================
// Calibration command construction
// ============================================================================

TEST_CASE("LoadCell calibration command construction", "[probe][loadcell][gcode]") {
    SECTION("Bare load_cell -> no LOAD_CELL= parameter") {
        REQUIRE(build_loadcell_command("LOAD_CELL_TARE", "load_cell") == "LOAD_CELL_TARE");
        REQUIRE(build_loadcell_command("LOAD_CELL_CALIBRATE", "load_cell") ==
                "LOAD_CELL_CALIBRATE");
        REQUIRE(build_loadcell_command("LOAD_CELL_DIAGNOSTIC", "load_cell") ==
                "LOAD_CELL_DIAGNOSTIC");
    }

    SECTION("Named load cell -> appends LOAD_CELL=<name>") {
        REQUIRE(build_loadcell_command("LOAD_CELL_TARE", "my_cell") ==
                "LOAD_CELL_TARE LOAD_CELL=my_cell");
        REQUIRE(build_loadcell_command("LOAD_CELL_CALIBRATE", "my_cell") ==
                "LOAD_CELL_CALIBRATE LOAD_CELL=my_cell");
        REQUIRE(build_loadcell_command("LOAD_CELL_DIAGNOSTIC", "my_cell") ==
                "LOAD_CELL_DIAGNOSTIC LOAD_CELL=my_cell");
    }

    SECTION("load_cell_probe sensor name treated as default (no parameter)") {
        REQUIRE(build_loadcell_command("LOAD_CELL_CALIBRATE", "load_cell_probe") ==
                "LOAD_CELL_CALIBRATE");
    }
}

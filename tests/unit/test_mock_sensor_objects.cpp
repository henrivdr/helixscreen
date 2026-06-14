// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_sensor_objects.cpp
 * @brief Verify that MoonrakerClientMock provides expected sensor objects for discovery
 *
 * The mock backend is the sole source of sensor data in mock mode. These tests
 * ensure populate_capabilities() includes the objects each sensor manager needs
 * for discovery (temperature sensors, humidity sensors, width sensor, probe,
 * filament sensors).
 */

#include "moonraker_client_mock.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helpers
// ============================================================================

/// Check if any object in the list matches a prefix and contains a substring
static bool has_object_matching(const std::vector<std::string>& objects, const std::string& prefix,
                                const std::string& substring) {
    return std::any_of(objects.begin(), objects.end(), [&](const std::string& obj) {
        return obj.rfind(prefix, 0) == 0 && obj.find(substring) != std::string::npos;
    });
}

/// Check if an exact object name exists in the list
static bool has_object(const std::vector<std::string>& objects, const std::string& name) {
    return std::find(objects.begin(), objects.end(), name) != objects.end();
}

// ============================================================================
// Test Fixture
// ============================================================================

class MockSensorObjectsFixture {
  public:
    MockSensorObjectsFixture()
        : client_(MoonrakerClientMock::PrinterType::VORON_24), hw_(client_.hardware()) {}

  protected:
    const std::vector<std::string>& objects() const {
        return hw_.printer_objects();
    }

    MoonrakerClientMock client_;
    helix::PrinterDiscovery hw_;
};

// ============================================================================
// Temperature Sensor Objects
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture,
                 "Mock backend provides temperature sensor objects for discovery",
                 "[mock][sensors][temperature]") {
    SECTION("Chamber temperature sensor is present") {
        REQUIRE(has_object_matching(objects(), "temperature_sensor ", "chamber"));
    }

    SECTION("MCU temperature sensor is present") {
        REQUIRE(has_object_matching(objects(), "temperature_sensor ", "mcu"));
    }

    SECTION("Host temperature sensor is present") {
        REQUIRE(has_object_matching(objects(), "temperature_sensor ", "raspberry"));
    }
}

// ============================================================================
// Humidity Sensor Objects
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture,
                 "Mock backend provides humidity sensor objects for discovery",
                 "[mock][sensors][humidity]") {
    SECTION("BME280 chamber sensor is present") {
        REQUIRE(has_object(objects(), "bme280 chamber"));
    }

    SECTION("HTU21D dryer sensor is present") {
        REQUIRE(has_object(objects(), "htu21d dryer"));
    }
}

// ============================================================================
// Width Sensor Object
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture,
                 "Mock backend provides width sensor object for discovery",
                 "[mock][sensors][width]") {
    REQUIRE(has_object(objects(), "hall_filament_width_sensor"));
}

// ============================================================================
// Probe Object
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture, "Mock backend provides probe object for discovery",
                 "[mock][sensors][probe]") {
    // Default probe type is cartographer; env var HELIX_MOCK_PROBE_TYPE can override.
    // At minimum, one probe-related object should exist.
    const auto& objs = objects();
    bool has_probe = has_object(objs, "probe") || has_object(objs, "cartographer") ||
                     has_object(objs, "bltouch") || has_object(objs, "beacon");
    REQUIRE(has_probe);
}

// ============================================================================
// Filament Sensor Objects
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture,
                 "Mock backend provides filament sensor objects for discovery",
                 "[mock][sensors][filament]") {
    REQUIRE(has_object_matching(objects(), "filament_switch_sensor ", ""));
}

// ============================================================================
// exclude_object Capability
// ============================================================================

TEST_CASE_METHOD(MockSensorObjectsFixture,
                 "Mock backend exposes exclude_object for capability discovery",
                 "[mock][sensors][exclude_object]") {
    SECTION("exclude_object object is present in the list") {
        REQUIRE(has_object(objects(), "exclude_object"));
    }

    SECTION("PrinterDiscovery flags has_exclude_object from the mock objects") {
        // The fixture's hardware() already parsed the mock object list, so the
        // capability flag (which gates adaptive bed mesh) must be set.
        REQUIRE(hw_.has_exclude_object());
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_validator_roles.cpp
 * @brief TDD tests for registry-driven role validation in HardwareValidator.
 *
 * Validates that a stale role pointing at a hardware/optional object is
 * surfaced (AutoHealed→newly_discovered, Unresolved→expected_missing) instead
 * of being silently dropped by the is_hardware_optional early-skip.
 *
 * Populate pattern: MoonrakerClientMock::set_fans/set_heaters + client.hardware()
 * Config pattern:   local Config with v3 format via setup_printer_data()
 */

#include "config.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "moonraker_client_mock.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// ---------------------------------------------------------------------------
// Local test fixture (separate name from helix::HardwareValidatorConfigFixture
// in test_hardware_validator.cpp to avoid ODR collision)
// ---------------------------------------------------------------------------
namespace helix {
class RoleValidatorFixture {
  protected:
    Config config;
    MoonrakerClientMock client;

    void setup_printer_data(const json& printer_data) {
        config.data = {{"config_version", 3},
                       {"active_printer_id", "default"},
                       {"printers", {{"default", printer_data}}}};
        config.active_printer_id_ = "default";
    }

    void setup_minimal(const json& fans_node = json::object(),
                       const json& heaters_node = json::object(),
                       const json& optional_list = json::array()) {
        json printer = {{"moonraker_host", "127.0.0.1"},
                        {"moonraker_port", 7125},
                        {"hardware",
                         {{"optional", optional_list},
                          {"expected", json::array()},
                          {"last_snapshot", json::object()}}}};
        if (!fans_node.empty())
            printer["fans"] = fans_node;
        if (!heaters_node.empty())
            printer["heaters"] = heaters_node;
        setup_printer_data(printer);
    }
};
} // namespace helix

// ---------------------------------------------------------------------------
// AutoHealed: stale role → canonical default present in discovered
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "validator surfaces stale part-fan role even when target is optional",
                 "[hwvalidate][roles]") {
    // Stale role: fans/part was set to "output_pin fan0" (now gone).
    // The user silenced "output_pin fan0" as hardware/optional.
    // A canonical "fan" IS available in discovery — AutoHeal path.
    setup_minimal(
        /*fans_node=*/{{"part", "output_pin fan0"}},
        /*heaters_node=*/json::object(),
        /*optional_list=*/{"output_pin fan0"});

    // Discovery: "output_pin fan0" is absent; "fan" (canonical default) is live.
    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan", "fan_generic Aux_Cooling_Fan"});

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // The blind spot is closed: the auto-heal must appear in newly_discovered
    // as hardware_name == "fan" (the canonical replacement chosen by resolve_role).
    bool mentions_part = false;
    for (const auto& issue : result.newly_discovered)
        if (issue.hardware_name == "fan" && issue.hardware_type == HardwareType::FAN)
            mentions_part = true;
    REQUIRE(mentions_part);
}

// ---------------------------------------------------------------------------
// Unresolved: stale role → no confident substitute
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "validator reports unresolved stale part-fan role to expected_missing",
                 "[hwvalidate][roles]") {
    // Stale role: fans/part was "output_pin fan0" which no longer exists.
    // No fans at all are discovered, so guess_part_cooling_fan() returns ""
    // (no fallback candidate) → Unresolved → expected_missing.
    setup_minimal(
        /*fans_node=*/{{"part", "output_pin fan0"}},
        /*heaters_node=*/json::object(),
        /*optional_list=*/{});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({}); // empty: no canonical "fan", no heuristic match

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    bool found_missing = false;
    for (const auto& issue : result.expected_missing)
        if (issue.hardware_name == "output_pin fan0" && issue.hardware_type == HardwareType::FAN)
            found_missing = true;
    REQUIRE(found_missing);
}

// ---------------------------------------------------------------------------
// Resolved: configured role still live → no issue emitted
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture, "validator emits no issue when all roles resolve",
                 "[hwvalidate][roles]") {
    setup_minimal(
        /*fans_node=*/{{"part", "fan"}, {"hotend", "heater_fan hotend_fan"}},
        /*heaters_node=*/{{"bed", "heater_bed"}, {"hotend", "extruder"}},
        /*optional_list=*/{});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan", "heater_fan hotend_fan"});

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // No roles in expected_missing or newly_discovered from the registry loop.
    REQUIRE(result.expected_missing.empty());
    for (const auto& issue : result.expected_missing) {
        INFO("unexpected expected_missing: " << issue.hardware_name);
        REQUIRE(issue.hardware_type == HardwareType::OTHER); // only AMS/generic, not Fan/Heater
    }
    bool any_role_issue = false;
    for (const auto& issue : result.newly_discovered)
        if (issue.hardware_type == HardwareType::FAN || issue.hardware_type == HardwareType::HEATER)
            any_role_issue = true;
    REQUIRE_FALSE(any_role_issue);
}

// ---------------------------------------------------------------------------
// Optional bypass: stale heater role also not silenced by optional flag
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(helix::RoleValidatorFixture,
                 "validator surfaces stale hotend-heater role even when target is optional",
                 "[hwvalidate][roles]") {
    // Configured hotend heater is a non-standard name that's gone; extruder (canonical) is live.
    setup_minimal(
        /*fans_node=*/json::object(),
        /*heaters_node=*/{{"hotend", "heater_generic custom_nozzle"}},
        /*optional_list=*/{"heater_generic custom_nozzle"});

    client.set_heaters({"extruder", "heater_bed"});
    client.set_fans({"fan"});

    HardwareValidator v;
    auto result = v.validate(&config, client.hardware());

    // AutoHealed: "extruder" is canonical default and is live.
    bool auto_healed = false;
    for (const auto& issue : result.newly_discovered)
        if (issue.hardware_name == "extruder" && issue.hardware_type == HardwareType::HEATER)
            auto_healed = true;
    REQUIRE(auto_healed);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "../../include/moonraker_client_mock.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "../test_helpers/temperature_controller_test_access.h"

#include "../catch_amalgamated.hpp"

using helix::HeaterType;
using helix::TemperatureController;

namespace {
struct ControllerFixture {
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPI api;
    TemperatureController controller;

    ControllerFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state),
          controller(state, &api) {
        state.init_subjects(false);
    }
};
} // namespace

TEST_CASE("TemperatureController resolves heater names", "[temp_controller]") {
    ControllerFixture f;

    SECTION("bed is heater_bed") {
        REQUIRE(f.controller.resolved_name(HeaterType::Bed) == "heater_bed");
    }
    SECTION("chamber uses the resolved discovery name, never the bare default") {
        // Drive the REAL resolution path: PrinterState::set_hardware() resolves the
        // chamber heater from discovery into temperature_state_, exactly as it does
        // in production. "auto" assignment makes resolution use the discovery name.
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"heater_generic chamber_heater", "extruder", "heater_bed"};
        hardware.parse_objects(objects);
        f.state.set_hardware(hardware);

        REQUIRE(f.controller.resolved_name(HeaterType::Chamber) ==
                "heater_generic chamber_heater");
    }
    SECTION("nozzle is the active extruder") {
        REQUIRE(f.controller.resolved_name(HeaterType::Nozzle) ==
                f.state.active_extruder_name());
    }
}

TEST_CASE("TemperatureController clamps keypad range to configured max", "[temp_controller]") {
    ControllerFixture f;

    SECTION("unknown configured max falls back to the heater default") {
        // Nozzle default ceiling is 350; nothing fetched yet.
        REQUIRE(f.controller.keypad_range(HeaterType::Nozzle).max == 350.0f);
    }
    SECTION("known configured max wins") {
        helix::TemperatureControllerTestAccess::set_max(f.controller, HeaterType::Chamber, 50);
        REQUIRE(f.controller.keypad_range(HeaterType::Chamber).max == 50.0f);
        REQUIRE(f.controller.configured_max(HeaterType::Chamber) == 50);
    }
}

TEST_CASE("TemperatureController preset visibility honors configured max", "[temp_controller]") {
    ControllerFixture f;
    helix::TemperatureControllerTestAccess::set_max(f.controller, HeaterType::Chamber, 50);
    REQUIRE(f.controller.preset_visible(HeaterType::Chamber, 40));
    REQUIRE(f.controller.preset_visible(HeaterType::Chamber, 50));
    REQUIRE_FALSE(f.controller.preset_visible(HeaterType::Chamber, 60));
    REQUIRE(f.controller.presets(HeaterType::Chamber).abs == 60); // value still defined
}

TEST_CASE("TemperatureController set_target routes to the resolved name", "[temp_controller]") {
    // Verify that set_target(HeaterType::Chamber, ...) sends the RESOLVED heater name
    // (e.g. "heater_generic chamber_heater" → bare object name "chamber_heater" in the
    // gcode), never the bare klipper type prefix that would be rejected by the firmware.
    ControllerFixture f;

    // Resolve chamber via the production path: PrinterState::set_hardware resolves the
    // chamber heater from discovery into temperature_state_, same as production.
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    helix::PrinterDiscovery hardware;
    nlohmann::json objects = {"heater_generic chamber_heater", "extruder", "heater_bed"};
    hardware.parse_objects(objects);
    f.state.set_hardware(hardware);

    // Confirm resolution before we exercise set_target
    REQUIRE(f.controller.resolved_name(HeaterType::Chamber) == "heater_generic chamber_heater");

    // execute_gcode gates on klippy state; the LVGL klippy_state subject defaults to
    // SHUTDOWN, so drive it to READY before exercising the gcode path.
    f.state.set_klippy_state_sync(helix::KlippyState::READY);

    // End-to-end check: set_target(HeaterType::Chamber, ...) passes the *resolved* chamber
    // name ("heater_generic chamber_heater", NOT the bare "chamber") down to
    // api_->set_temperature. The controller does NOT manipulate the name itself — the gcode
    // layer (build_heater_gcode inside MoonrakerAPI) strips the "heater_generic " prefix to
    // produce a valid "HEATER=chamber_heater". We can only observe the result: on_success
    // fires and on_error does not, which confirms the gcode layer received a usable
    // resolved name and emitted a valid HEATER= value (the mock returns an error for a
    // malformed one).
    bool success_fired = false;
    bool error_fired = false;
    f.controller.set_target(HeaterType::Chamber, 45.0,
                            helix::SendOptions{.toast = false,
                                              .on_success = [&] { success_fired = true; },
                                              .on_error = [&](const MoonrakerError&) {
                                                  error_fired = true;
                                              }});

    REQUIRE(success_fired);
    REQUIRE_FALSE(error_fired);

    // Verify the RPC method used was printer.gcode.script (confirms the gcode path ran)
    REQUIRE(f.client.last_send_method() == "printer.gcode.script");
}

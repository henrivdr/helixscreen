// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "../../include/moonraker_client_mock.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"

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

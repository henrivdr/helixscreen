// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "printer_state.h"

namespace helix {

TemperatureController::TemperatureController(PrinterState& state, MoonrakerAPI* api)
    : state_(state), api_(api) {}

std::string TemperatureController::resolved_name(HeaterType type) const {
    switch (type) {
    case HeaterType::Nozzle:
        return state_.active_extruder_name();
    case HeaterType::Bed:
        return "heater_bed";
    case HeaterType::Chamber:
        return state_.temperature_state().chamber_heater_name();
    }
    return "";
}

} // namespace helix

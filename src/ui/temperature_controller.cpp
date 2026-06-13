// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "temperature_controller.h"

#include "filament_database.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "ui_error_reporting.h"

#include "hv/json.hpp"
#include "lvgl/src/others/translation/lv_translation.h"

#include <algorithm>
#include <cctype>

namespace helix {

TemperatureController::TemperatureController(PrinterState& state, MoonrakerAPI* api)
    : state_(state), api_(api) {
    // Keypad ceilings mirror temperature_service.cpp keypad_range fields.
    model_[idx(HeaterType::Nozzle)].keypad_max_default = 350.0f;
    model_[idx(HeaterType::Bed)].keypad_max_default = 150.0f;
    model_[idx(HeaterType::Chamber)].keypad_max_default = 80.0f;

    // Preset values mirror temperature_service.cpp TemperatureService ctor.
    // Single source of truth: nozzle/bed presets are derived from the filament
    // database (same derivation as temperature_service.cpp lines 67-80), so
    // views reading presets from the controller match what the service produced.
    auto pla_info = filament::find_material("PLA");
    auto petg_info = filament::find_material("PETG");
    auto abs_info = filament::find_material("ABS");

    // Nozzle presets (fallbacks 210/245/255 match the service)
    int nozzle_pla = pla_info ? pla_info->nozzle_recommended() : 210;
    int nozzle_petg = petg_info ? petg_info->nozzle_recommended() : 245;
    int nozzle_abs = abs_info ? abs_info->nozzle_recommended() : 255;

    // Bed presets (fallbacks 60/80/100 match the service)
    int bed_pla = pla_info ? pla_info->bed_temp : 60;
    int bed_petg = petg_info ? petg_info->bed_temp : 80;
    int bed_abs = abs_info ? abs_info->bed_temp : 100;

    model_[idx(HeaterType::Nozzle)].presets = {
        .off = 0, .pla = nozzle_pla, .petg = nozzle_petg, .abs = nozzle_abs};
    model_[idx(HeaterType::Bed)].presets = {
        .off = 0, .pla = bed_pla, .petg = bed_petg, .abs = bed_abs};
    // Chamber: hardcoded {0, 40, 50, 60} in temperature_service.cpp.
    model_[idx(HeaterType::Chamber)].presets = {.off = 0, .pla = 40, .petg = 50, .abs = 60};
}

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

int TemperatureController::configured_max(HeaterType type) const {
    return model_[idx(type)].configured_max;
}

void TemperatureController::set_configured_max(HeaterType type, int deg) {
    model_[idx(type)].configured_max = deg;
}

KeypadRange TemperatureController::keypad_range(HeaterType type) const {
    const auto& m = model_[idx(type)];
    return {m.keypad_min, heater_effective_max_deg(m.keypad_max_default, m.configured_max)};
}

void TemperatureController::ensure_limits(HeaterType type) {
    if (!api_ || model_[idx(type)].configured_max > 0) {
        return;
    }
    std::string section = resolved_name(type);
    if (section.empty()) {
        return;
    }
    // configfile.config section headers are lower-cased by Moonraker; lower-case
    // defensively to match regardless of how the discovery name was capitalised.
    std::transform(section.begin(), section.end(), section.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto tok = lifetime_.token();
    api_->query_configfile(
        [this, tok, type, section](const nlohmann::json& config) {
            // Background (WS) thread: parse only — no `this` member access.
            int max_deg = 0;
            if (config.contains(section)) {
                const auto& sec = config[section];
                if (sec.contains("max_temp")) {
                    const auto& mt = sec["max_temp"];
                    try {
                        if (mt.is_string()) {
                            max_deg = static_cast<int>(std::stof(mt.get<std::string>()));
                        } else if (mt.is_number()) {
                            max_deg = static_cast<int>(mt.get<double>());
                        }
                    } catch (const std::exception&) {
                        max_deg = 0;
                    }
                }
            }
            if (max_deg <= 0) {
                return; // no usable ceiling — keep the heater default
            }
            // Main thread: mutate state.
            tok.defer("TemperatureController::apply_max",
                      [this, type, max_deg]() { set_configured_max(type, max_deg); });
        },
        [](const MoonrakerError&) {});
}

const HeaterPresets& TemperatureController::presets(HeaterType type) const {
    return model_[idx(type)].presets;
}

bool TemperatureController::preset_visible(HeaterType type, int value_c) const {
    return heater_preset_visible(value_c, model_[idx(type)].configured_max);
}

void TemperatureController::set_target(HeaterType type, double celsius, SendOptions opts) {
    const std::string name = resolved_name(type);
    if (name.empty()) {
        // Only the chamber resolves empty in practice (nozzle -> active extruder,
        // bed -> "heater_bed" are always present). Surface the not-found condition
        // only when the caller wants user-visible feedback; silent sends
        // (toast=false, e.g. AMS slot-preheat / cooldown) stay a clean no-op.
        // Mirrors the gcode-send error path: fire on_error if provided, then toast.
        if (opts.toast) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = (type == HeaterType::Chamber) ? "Chamber heater not found"
                                                        : "Heater not found";
            if (opts.on_error) {
                opts.on_error(err);
            }
            NOTIFY_ERROR("{}", lv_tr(err.message.c_str()));
        }
        return;
    }
    set_target(name, celsius, std::move(opts));
}

void TemperatureController::set_target(const std::string& klipper_name, double celsius,
                                       SendOptions opts) {
    if (!api_ || klipper_name.empty()) {
        return;
    }
    auto on_ok = [opts]() {
        if (opts.on_success)
            opts.on_success();
    };
    auto on_err = [opts](const MoonrakerError& e) {
        if (opts.on_error)
            opts.on_error(e);
        if (opts.toast) {
            NOTIFY_ERROR(lv_tr("Failed to set temperature: {}"), e.user_message());
        }
    };
    api_->set_temperature(klipper_name, celsius, std::move(on_ok), std::move(on_err));
}

void TemperatureController::apply_material(double nozzle, double bed, double chamber,
                                           SendOptions opts) {
    set_target(HeaterType::Nozzle, nozzle, opts);
    set_target(HeaterType::Bed, bed, opts);
    const std::string chamber_name = resolved_name(HeaterType::Chamber);
    if (chamber > 0 && !chamber_name.empty()) {
        set_target(chamber_name, chamber, opts);
    }
}

} // namespace helix

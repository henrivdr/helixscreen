// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_registry.h"

#include "app_globals.h" // get_moonraker_api()
#include "config.h"      // helix::Config

#include "ui_wizard_ams_identify.h"
#include "ui_wizard_connection.h"
#include "ui_wizard_fan_select.h"
#include "ui_wizard_filament_sensor_select.h"
#include "ui_wizard_heater_select.h"
#include "ui_wizard_input_shaper.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_led_select.h"
#include "ui_wizard_printer_identify.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

namespace helix {
namespace wizard {

const std::vector<Step*>& steps() {
    // Built once, in StepId enum order (index 0..kStepCount-1).
    static const std::vector<Step*> kSteps = [] {
        std::vector<Step*> v;
        v.reserve(kStepCount);
        v.push_back(get_wizard_touch_calibration_step());     // TouchCalibration = 0
        v.push_back(get_wizard_language_chooser_step());       // Language
        v.push_back(get_wizard_wifi_step());                  // Wifi
        v.push_back(get_wizard_connection_step());             // Connection
        v.push_back(get_wizard_printer_identify_step());       // PrinterIdentify
        v.push_back(get_wizard_heater_select_step());          // HeaterSelect
        v.push_back(get_wizard_fan_select_step());             // FanSelect
        v.push_back(get_wizard_ams_identify_step());           // AmsIdentify
        v.push_back(get_wizard_led_select_step());             // LedSelect
        v.push_back(get_wizard_filament_sensor_select_step()); // FilamentSensor
        v.push_back(get_wizard_input_shaper_step());           // InputShaper
        v.push_back(get_wizard_summary_step());                // Summary
        v.push_back(get_wizard_telemetry_step());              // Telemetry
        return v;
    }();
    return kSteps;
}

Step* step_by_id(StepId id) {
    const auto& v = steps();
    const int i = static_cast<int>(id);
    if (i < 0 || i >= static_cast<int>(v.size())) {
        return nullptr;
    }
    return v[i];
}

StepContext build_context() {
    StepContext c;
    c.config = helix::Config::get_instance();
    c.api = get_moonraker_api();
    int n = c.config ? static_cast<int>(c.config->get_printer_ids().size()) : 1;
    bool has_preset = c.config && c.config->has_preset();
    c.preset = helix::wizard_preset_plan(has_preset, n);
    c.is_subsequent_printer = n > 1;
    c.is_fbdev = false;            // steps that need it delegate to their own legacy check
    c.force_language_step = false; // same
    return c;
}

std::vector<helix::StepSkip> skip_vector(const StepContext& ctx) {
    std::vector<helix::StepSkip> out;
    const auto& v = steps();
    out.reserve(v.size());
    for (Step* s : v) {
        out.push_back({s->id(), s->should_skip(ctx)});
    }
    return out;
}

} // namespace wizard
} // namespace helix

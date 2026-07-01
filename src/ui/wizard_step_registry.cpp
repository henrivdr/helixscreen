// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard_step_registry.h"

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

#include "app_globals.h" // get_moonraker_api()
#include "config.h"      // helix::Config

namespace helix {
namespace wizard {

std::vector<Step*> steps() {
    // Rebuilt on every call — DO NOT cache the Step* in a static vector.
    // The step singletons are lazily (re)created by their get_wizard_*_step()
    // accessors and are destroyed by StaticPanelRegistry when the wizard tears
    // down. A cached raw-pointer vector dangled across that teardown: adding a
    // 3rd printer after the first wizard completed made a virtual call on a freed
    // Step, crashing (SIGSEGV, vtable jump to heap). The accessors recreate a
    // destroyed singleton on demand, so fetching fresh is always valid + cheap.
    std::vector<Step*> v;
    v.reserve(kStepCount);
    v.push_back(get_wizard_touch_calibration_step());      // TouchCalibration = 0
    v.push_back(get_wizard_language_chooser_step());       // Language
    v.push_back(get_wizard_wifi_step());                   // Wifi
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
}

Step* step_by_id(StepId id) {
    const int i = static_cast<int>(id);
    if (i < 0 || i >= kStepCount) {
        return nullptr;
    }
    return steps()[i];
}

StepContext build_context() {
    StepContext c;
    c.config = helix::Config::get_instance();
    c.api = get_moonraker_api();
    int n = c.config ? static_cast<int>(c.config->get_printer_ids().size()) : 1;
    bool has_preset = c.config && c.config->has_preset();
    c.preset = helix::wizard_preset_plan(has_preset, n);
    c.is_subsequent_printer = n > 1;
    return c;
}

std::vector<helix::StepSkip> skip_vector(const StepContext& ctx) {
    std::vector<helix::StepSkip> out;
    const std::vector<Step*> v = steps();
    out.reserve(v.size());
    for (Step* s : v) {
        out.push_back({s->id(), s->should_skip(ctx)});
    }
    return out;
}

} // namespace wizard
} // namespace helix

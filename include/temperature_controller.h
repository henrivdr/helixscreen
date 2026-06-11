// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "moonraker_error.h"
#include "ui_heater_config.h" // helix::HeaterType

#include <functional>
#include <string>

namespace helix {
class PrinterState;
}
class MoonrakerAPI;

namespace helix {

struct SendOptions {
    bool toast = true;
    bool silent = false;
    std::function<void()> on_success = nullptr;
    std::function<void(const MoonrakerError&)> on_error = nullptr;
};

struct KeypadRange {
    float min = 0.0f;
    float max = 0.0f;
};

/// Single authority for heater target control: name resolution, configured-max
/// limits, presets, and the one send. No LVGL widgets/subjects — uses the
/// NOTIFY_* notification system for toasts, so logic is unit-testable.
class TemperatureController {
  public:
    TemperatureController(PrinterState& state, MoonrakerAPI* api);

    void set_api(MoonrakerAPI* api) {
        api_ = api;
    }

    /// Klipper object name to target. Nozzle -> active extruder; Bed ->
    /// "heater_bed"; Chamber -> resolved discovery name (never the bare default).
    std::string resolved_name(HeaterType type) const;

  private:
    PrinterState& state_;
    MoonrakerAPI* api_;
};

} // namespace helix

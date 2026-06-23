// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_heater_config.h" // helix::HeaterType, HEATER_TYPE_COUNT

#include "async_lifetime_guard.h"
#include "heater_limits.h"
#include "moonraker_error.h"

#include <array>
#include <functional>
#include <string>

namespace helix {
class PrinterState;
}
class MoonrakerAPI;

namespace helix {

/// Preset target temperatures (°C) for a single heater.
struct HeaterPresets {
    int off = 0;
    int pla = 0;
    int petg = 0;
    int abs = 0;
};

/// Options for a heater set-target call.
/// - toast: show the standard error toast on failure (default true).
/// - on_success / on_error: optional caller hooks fired after the RPC completes.
struct SendOptions {
    bool toast = true;
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

    /// Klipper-configured max_temp in °C, or 0 if not yet fetched.
    int configured_max(HeaterType type) const;

    /// Keypad input range: min..effective ceiling (configured max if known,
    /// otherwise the heater default).
    KeypadRange keypad_range(HeaterType type) const;

    /// Fetch the Klipper configfile max_temp for this heater if not yet known.
    /// No-op if api_ is null or the value is already populated.
    void ensure_limits(HeaterType type);

    /// The heater's preset target values (°C).
    const HeaterPresets& presets(HeaterType type) const;

    /// Whether a preset value should be shown given the configured max (hidden if above it).
    bool preset_visible(HeaterType type, int value_c) const;

    /// Send a temperature target by heater type.  Resolves to the klipper object name first.
    void set_target(HeaterType type, double celsius, SendOptions opts = {});

    /// Send a temperature target by explicit klipper object name (e.g. "heater_generic
    /// chamber_heater" or "extruder").  Returns immediately if api_ is null or name is empty.
    void set_target(const std::string& klipper_name, double celsius, SendOptions opts = {});

    /// Send nozzle/bed/chamber targets in a single call.  Chamber is skipped when its resolved
    /// name is empty or chamber == 0.
    void apply_material(double nozzle, double bed, double chamber, SendOptions opts = {});

  private:
    friend struct TemperatureControllerTestAccess;
    void set_configured_max(HeaterType type, int deg);

    struct HeaterModel {
        float keypad_min = 0.0f;
        float keypad_max_default = 0.0f; // 350 nozzle / 150 bed / 80 chamber
        int configured_max = 0;          // °C from configfile, 0 = unknown
        HeaterPresets presets{};
    };
    std::array<HeaterModel, HEATER_TYPE_COUNT> model_{};
    AsyncLifetimeGuard lifetime_;

    static int idx(HeaterType t) {
        return static_cast<int>(t);
    }

    PrinterState& state_;
    MoonrakerAPI* api_;
};

} // namespace helix

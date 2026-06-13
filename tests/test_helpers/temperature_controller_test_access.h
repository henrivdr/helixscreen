// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "temperature_controller.h"
namespace helix {
struct TemperatureControllerTestAccess {
    static void set_max(TemperatureController& c, HeaterType t, int deg) {
        c.set_configured_max(t, deg);
    }
};
} // namespace helix

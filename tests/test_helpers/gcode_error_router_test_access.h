// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_error_router.h"

#include <string>

// Test-only friend that exposes GcodeErrorRouter's private presentation glue.
// process_line() is the seam between the (already unit-tested) pure routing
// decision and the LVGL/notification surfacing; reaching it directly lets the
// e2e test drive a raw `!!` line straight to a modal without standing up a
// MoonrakerClient + WebSocket. See gcode_error_router.h (friend declaration).
struct GcodeErrorRouterTestAccess {
    static void process_line(helix::GcodeErrorRouter& r, const std::string& line) {
        r.process_line(line);
    }
};

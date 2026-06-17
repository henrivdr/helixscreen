// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "display_backend.h"
#include "display_manager.h"

#include <memory>

// Test-only seam (#1049). DisplayManager declares this class as a friend, so the
// statics below can reach its private members to exercise the sleep/wake/power-off
// paths without a full init() (LVGLTestFixture already owns LVGL). Keeping these
// out of the production header satisfies the "no _for_testing methods in headers"
// lint (L065/L088).
class DisplayManagerTestAccess {
  public:
    static void set_backend(DisplayManager& dm, std::unique_ptr<DisplayBackend> backend) {
        dm.m_backend = std::move(backend);
    }

    static void set_use_hardware_blank(DisplayManager& dm, bool use_hw) {
        dm.m_use_hardware_blank = use_hw;
    }

    static void set_use_power_off(DisplayManager& dm, bool use_power_off) {
        dm.m_use_power_off = use_power_off;
    }

    static void enter_sleep(DisplayManager& dm, int timeout_sec) {
        dm.enter_sleep(timeout_sec);
    }

    // Exercises the wake-side panel restore (power-on / unblank / overlay removal)
    // WITHOUT the post-wake lv_refr_now(), which infinite-loops under the headless
    // test display. Production wake_display() runs this then lv_refr_now().
    static void restore_display_output(DisplayManager& dm) {
        dm.m_display_sleeping = false;
        dm.restore_display_output();
    }
};

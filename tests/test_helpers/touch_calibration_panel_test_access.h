// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "touch_calibration_panel.h"

#include <cstdint>
#include <functional>

namespace helix {

/**
 * @brief Test-only access to TouchCalibrationPanel debounce internals.
 *
 * The press-debounce gate (`debounce_enabled_`) is normally seeded once from
 * the process-global `RuntimeConfig::touch_cal_debounce()` static cache. That
 * cache is order-dependent across a test binary, so tests must NOT rely on the
 * env var to toggle it. This access class provides deterministic seams:
 *
 *  - `set_debounce_enabled()` overrides the gate per-panel-instance.
 *  - `set_now_fn()` injects a monotonic-ms clock so the refractory + stall
 *    windows can be advanced synchronously without spinning a real timer.
 */
class TouchCalibrationPanelTestAccess {
  public:
    static void set_debounce_enabled(TouchCalibrationPanel& p, bool enabled) {
        p.debounce_enabled_ = enabled;
    }

    static bool debounce_enabled(const TouchCalibrationPanel& p) {
        return p.debounce_enabled_;
    }

    static bool press_pending(const TouchCalibrationPanel& p) {
        return p.press_pending_;
    }

    /// Inject a deterministic monotonic-ms clock used for refractory + stall logic.
    static void set_now_fn(TouchCalibrationPanel& p, std::function<uint32_t()> fn) {
        p.now_fn_ = std::move(fn);
    }

    /// Drive the stall-timeout fallback directly (the LVGL timer would otherwise
    /// fire this). Commits a pending press outstanding >= STALL_COMMIT_MS.
    static void commit_pending_if_stale(TouchCalibrationPanel& p, uint32_t now) {
        p.commit_pending_if_stale(now);
    }

    static uint32_t refractory_ms() {
        return TouchCalibrationPanel::REFRACTORY_MS;
    }

    static uint32_t stall_commit_ms() {
        return TouchCalibrationPanel::STALL_COMMIT_MS;
    }

    static int samples_required() {
        return TouchCalibrationPanel::SAMPLES_REQUIRED;
    }
};

} // namespace helix

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "touch_calibration.h"

#include <lvgl.h>

namespace helix {

/// Context for the calibrated touch read callback wrapper.
/// Stored as lv_indev user_data. Lifetime must match the backend object.
struct CalibrationContext {
    TouchCalibration calibration;
    lv_indev_read_cb_t original_read_cb = nullptr;
    int screen_width = 800;
    int screen_height = 480;
};

/// Read callback wrapper that applies affine touch calibration.
/// Chains to original_read_cb first, then transforms coordinates.
void calibrated_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

/// Load stored touch calibration coefficients from Config.
/// Returns TouchCalibration with valid=false if none stored or invalid.
TouchCalibration load_touch_calibration();

/// Install the calibration read callback wrapper on an input device.
/// Sets up ctx with the calibration data, chains to the existing read callback.
/// Safe to call even when cal.valid is false (becomes a passthrough).
void install_calibration_wrapper(lv_indev_t* indev, CalibrationContext& ctx,
                                 const TouchCalibration& cal, int screen_w, int screen_h);

} // namespace helix

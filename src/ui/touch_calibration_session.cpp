// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "touch_calibration_session.h"

#include <spdlog/spdlog.h>

namespace helix {

void TouchCalibrationSession::begin_capture(ICalibrationSink& sink) {
    backup_ = sink.current_calibration();
    has_backup_ = true;
    sink.disable_affine();
    spdlog::debug("[TouchCalSession] begin_capture: backup snapshotted (valid={}), affine disabled",
                  backup_.valid);
}

void TouchCalibrationSession::revert_for_retry(ICalibrationSink& sink) {
    bool reverted = has_backup_ && backup_.valid;
    if (reverted) {
        sink.apply_calibration(backup_);
    }
    sink.disable_affine();
    spdlog::debug("[TouchCalSession] revert_for_retry: restored backup={}, affine disabled",
                  reverted);
}

void TouchCalibrationSession::commit() {
    has_backup_ = false;
    backup_ = {};
    spdlog::debug("[TouchCalSession] commit: backup dropped (new calibration kept)");
}

void TouchCalibrationSession::restore(ICalibrationSink& sink) {
    bool reverted = has_backup_ && backup_.valid;
    if (reverted) {
        sink.apply_calibration(backup_);
    }
    // Always re-enable the affine transform, even when no valid backup was held
    // (first-run wizard) or no session was active — touch must never be left in
    // the disabled state a capture session puts it in (#943).
    sink.enable_affine();
    has_backup_ = false;
    backup_ = {};
    spdlog::debug("[TouchCalSession] restore: restored backup={}, affine re-enabled", reverted);
}

} // namespace helix

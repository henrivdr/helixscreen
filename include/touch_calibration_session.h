// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#pragma once

#include "touch_calibration.h"

namespace helix {

/// The four touch-calibration operations an interactive calibration session
/// performs on the live input device. DisplayManager implements this; the
/// indirection exists so TouchCalibrationSession's backup/restore logic can be
/// unit-tested against a fake sink without a real display backend.
struct ICalibrationSink {
    virtual ~ICalibrationSink() = default;

    /// The calibration currently stored for the touch device (the affine that
    /// would be active if the affine transform were enabled).
    virtual TouchCalibration current_calibration() const = 0;

    /// Install a calibration as the active affine. Returns false if rejected
    /// (e.g. invalid coefficients).
    virtual bool apply_calibration(const TouchCalibration& cal) = 0;

    /// Disable the affine transform so the device reports pre-affine
    /// coordinates (used while capturing calibration points).
    virtual void disable_affine() = 0;

    /// Re-enable the affine transform using the device's stored calibration.
    virtual void enable_affine() = 0;
};

/// Shared backup/disable/restore bookkeeping for an interactive touch
/// calibration session, used by both the first-run wizard
/// (WizardTouchCalibrationStep) and the Settings recalibration overlay
/// (TouchCalibrationOverlay).
///
/// Centralizing it guarantees the one invariant that matters: however a session
/// ends — accepted, cancelled, timed out, or dismissed by navigating away — the
/// affine transform is re-enabled, so an aborted recalibration never leaves
/// touch uncalibrated until the next reboot (prestonbrown/helixscreen#943). The
/// Settings overlay previously re-enabled the affine only in cleanup(), which
/// the navigation stack never calls on a plain dismiss, so a failed recalibrate
/// left the panel's touch input disabled until a restart.
class TouchCalibrationSession {
  public:
    /// Snapshot the calibration active now and disable the affine transform so
    /// the session can capture raw (pre-affine) coordinates. Re-snapshots on
    /// every call: a session always begins from a clean baseline.
    void begin_capture(ICalibrationSink& sink);

    /// Revert to the snapshot (if a valid one is held) and disable the affine
    /// transform again, ready for another capture attempt (retry / timeout /
    /// fast-revert).
    void revert_for_retry(ICalibrationSink& sink);

    /// The user accepted/persisted a new calibration: drop the snapshot so a
    /// later restore() will not revert their new calibration.
    void commit();

    /// Re-enable the affine transform, reverting to the snapshot if one is still
    /// held. This is the teardown guarantee — idempotent and safe to call when
    /// no session is active.
    void restore(ICalibrationSink& sink);

    bool has_backup() const {
        return has_backup_;
    }

  private:
    TouchCalibration backup_{};
    bool has_backup_ = false;
};

} // namespace helix

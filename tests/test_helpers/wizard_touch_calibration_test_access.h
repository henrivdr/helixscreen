// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_wizard_touch_calibration.h"

#include "touch_calibration_panel.h"

/**
 * @brief Test-only access to WizardTouchCalibrationStep internals.
 *
 * The wizard owns a private TouchCalibrationPanel. Driving that panel directly
 * (via helix::TouchCalibrationPanelTestAccess) is the only way to exercise the
 * wizard's verify-entry auto-accept wiring (#1029) without standing up the full
 * LVGL/XML screen — the panel reaching COMPLETE proves the wizard wired
 * set_verify_entry_callback to accept() on the real commit path.
 *
 * Lives in the global namespace to match WizardTouchCalibrationStep (which is
 * not namespaced), so the `friend class WizardTouchCalibrationTestAccess;`
 * declaration in the wizard header resolves to this class.
 */
class WizardTouchCalibrationTestAccess {
  public:
    static helix::TouchCalibrationPanel* panel(WizardTouchCalibrationStep& step) {
        return step.panel_.get();
    }
};

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "printer_state.h"
#include "update_queue_test_access.h"

namespace helix {

class PrinterPrintStateTestAccess {
  public:
    static void reset_extra(PrinterPrintState& pps) {
        pps.estimated_print_time_ = 0;
        pps.has_real_layer_data_ = false;
        pps.slicer_progress_ = 0.0;
        pps.slicer_progress_active_ = false;
        pps.smoothed_remaining_ = 0.0;
        pps.has_smoothed_remaining_ = false;
        pps.sdcard_active_ = false;
    }
};

// PrinterStateTestAccess must be in namespace helix to match friend declaration in PrinterState
class PrinterStateTestAccess {
  public:
    static void reset(PrinterState& ps) {
        helix::ui::UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        ps.deinit_subjects();
        ps.printer_type_.clear();
        ps.pre_print_option_set_ = PrePrintOptionSet();
        ps.z_offset_calibration_strategy_ = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
        ps.auto_detected_bed_moves_ = false;
        ps.is_paused_ = false;
        ps.last_kinematics_.clear();
        PrinterPrintStateTestAccess::reset_extra(ps.print_domain_);
    }

    static PrinterFanState& get_fan_state(PrinterState& ps) {
        return ps.fan_state_;
    }

    /// Inject a synthetic pre-print option set (bypasses the printer DB) so tests
    /// can exercise option configurations that no shipped printer declares yet —
    /// e.g. a bed_mesh option with a custom adaptive_param name.
    static void set_option_set(PrinterState& ps, PrePrintOptionSet set) {
        ps.pre_print_option_set_ = std::move(set);
    }
};

} // namespace helix

using namespace helix;

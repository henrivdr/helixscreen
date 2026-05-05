// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "print_phase_tracker.h"

namespace helix {

/// Test friend giving direct access to PrintPhaseTracker internals so unit
/// tests can drive the gcode parser without spinning up Moonraker / LVGL
/// observers. The notify_gcode_response listener is the public attach path
/// in production; in tests we feed lines straight in.
class PrintPhaseTrackerTestAccess {
  public:
    static void process_line(PrintPhaseTracker& t, const std::string& line) {
        t.process_gcode_line(line);
    }

    static PrintPhase current_phase(PrintPhaseTracker& t) {
        return t.current_phase_;
    }

    static int mesh_probes_seen(PrintPhaseTracker& t) {
        return t.mesh_probes_seen_;
    }

    static int mesh_probes_total(PrintPhaseTracker& t) {
        return t.mesh_probes_total_;
    }

    static float mesh_seconds_per_probe(PrintPhaseTracker& t) {
        return t.mesh_seconds_per_probe_;
    }
};

} // namespace helix

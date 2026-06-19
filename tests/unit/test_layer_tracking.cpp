// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_layer_tracking.cpp
 * @brief Tests for layer tracking: print_stats.info primary path + gcode response fallback
 *
 * Verifies that print_layer_current_ subject is updated from both:
 * 1. Moonraker print_stats.info.current_layer (primary path via update_from_status)
 * 2. Gcode response parsing (fallback for slicers that don't emit SET_PRINT_STATS_INFO)
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_manager.h"
#include "printer_state.h"

#include <regex>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using json = nlohmann::json;

// ============================================================================
// Helper: parse a gcode response line for layer info (mirrors application.cpp logic)
// ============================================================================

namespace {

struct LayerParseResult {
    int layer = -1;
    int total = -1;
};

LayerParseResult parse_layer_from_gcode(const std::string& line) {
    LayerParseResult result;

    // Pattern 1: SET_PRINT_STATS_INFO CURRENT_LAYER=N [TOTAL_LAYER=N]
    if (line.find("SET_PRINT_STATS_INFO") != std::string::npos) {
        auto pos = line.find("CURRENT_LAYER=");
        if (pos != std::string::npos) {
            result.layer = std::atoi(line.c_str() + pos + 14);
        }
        pos = line.find("TOTAL_LAYER=");
        if (pos != std::string::npos) {
            result.total = std::atoi(line.c_str() + pos + 12);
        }
    }

    // Pattern 2: ;LAYER:N
    if (result.layer < 0 && line.size() >= 8 && line[0] == ';' && line[1] == 'L' &&
        line[2] == 'A' && line[3] == 'Y' && line[4] == 'E' && line[5] == 'R' && line[6] == ':') {
        result.layer = std::atoi(line.c_str() + 7);
    }

    return result;
}

} // namespace

// ============================================================================
// Primary path: print_stats.info.current_layer via update_from_status
// ============================================================================

TEST_CASE("Layer tracking: print_stats.info.current_layer updates subject",
          "[layer_tracking][print_stats]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    SECTION("current_layer updates from info object") {
        json status = {{"print_stats", {{"info", {{"current_layer", 5}, {"total_layer", 110}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 5);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 110);
    }

    SECTION("null info does not crash or update") {
        // Set initial value
        json status = {{"print_stats", {{"info", {{"current_layer", 3}}}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 3);

        // Send null info - should not change the value
        json null_info = {{"print_stats", {{"info", nullptr}}}};
        state.update_from_status(null_info);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 3);
    }

    SECTION("missing info key does not crash") {
        json status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(status);
        // Should still be at default (0)
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }
}

// ============================================================================
// Gcode response parsing (unit tests for the parsing logic)
// ============================================================================

TEST_CASE("Layer tracking: gcode response parsing", "[layer_tracking][gcode]") {
    SECTION("SET_PRINT_STATS_INFO CURRENT_LAYER=N parses correctly") {
        auto result = parse_layer_from_gcode("SET_PRINT_STATS_INFO CURRENT_LAYER=5");
        REQUIRE(result.layer == 5);
        REQUIRE(result.total == -1); // no total in this line
    }

    SECTION("SET_PRINT_STATS_INFO with both CURRENT_LAYER and TOTAL_LAYER") {
        auto result =
            parse_layer_from_gcode("SET_PRINT_STATS_INFO CURRENT_LAYER=3 TOTAL_LAYER=110");
        REQUIRE(result.layer == 3);
        REQUIRE(result.total == 110);
    }

    SECTION(";LAYER:N comment format (OrcaSlicer/PrusaSlicer/Cura)") {
        auto result = parse_layer_from_gcode(";LAYER:42");
        REQUIRE(result.layer == 42);
    }

    SECTION(";LAYER:0 parses zero layer") {
        auto result = parse_layer_from_gcode(";LAYER:0");
        REQUIRE(result.layer == 0);
    }

    SECTION("unrelated gcode lines are ignored") {
        auto result = parse_layer_from_gcode("ok");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("G1 X10 Y20 Z0.3");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("M104 S200");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode("");
        REQUIRE(result.layer == -1);
    }

    SECTION("short lines don't cause out-of-bounds") {
        auto result = parse_layer_from_gcode(";L");
        REQUIRE(result.layer == -1);

        result = parse_layer_from_gcode(";LAYER");
        REQUIRE(result.layer == -1);
    }
}

// ============================================================================
// set_print_layer_current setter (thread-safe path)
// ============================================================================

TEST_CASE("Layer tracking: set_print_layer_current setter", "[layer_tracking][setter]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("setter updates the subject via async") {
        state.set_print_layer_current(7);
        // Process the async queue so the value actually lands
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 7);
    }

    SECTION("setter and print_stats.info both update same subject") {
        // Simulate gcode fallback setting layer
        state.set_print_layer_current(10);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 10);

        // Then print_stats.info comes in with a different value (takes precedence naturally)
        json status = {{"print_stats", {{"info", {{"current_layer", 12}}}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 12);
    }

    SECTION("setter marks has_real_layer_data true") {
        REQUIRE_FALSE(state.has_real_layer_data());
        state.set_print_layer_current(5);
        // Flag is set inside the async lambda, so drain the queue first
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(state.has_real_layer_data());
    }
}

// ============================================================================
// virtual_sdcard.layer path (K1C and newer Klipper)
// ============================================================================

TEST_CASE("Layer tracking: virtual_sdcard.layer updates subject",
          "[layer_tracking][virtual_sdcard]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    SECTION("layer and layer_count update from virtual_sdcard") {
        json status = {{"virtual_sdcard", {{"progress", 0.50}, {"layer", 158}, {"layer_count", 296}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 158);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 296);
        REQUIRE(state.has_real_layer_data());
    }

    SECTION("virtual_sdcard.layer takes precedence over estimation") {
        // Set total for estimation to have something to work with
        state.set_print_layer_total(296);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Send progress with layer data — should use layer, not estimation
        json status = {{"virtual_sdcard", {{"progress", 0.66}, {"layer", 158}, {"layer_count", 296}}}};
        state.update_from_status(status);

        // 0.66 * 296 = 195 (estimation), but real layer is 158
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 158);
    }

    SECTION("virtual_sdcard.layer prevents future estimation") {
        json with_layer = {{"virtual_sdcard", {{"progress", 0.50}, {"layer", 100}}}};
        state.update_from_status(with_layer);
        REQUIRE(state.has_real_layer_data());

        // Further progress without layer data should NOT overwrite via estimation
        state.set_print_layer_total(296);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json without_layer = {{"virtual_sdcard", {{"progress", 0.80}}}};
        state.update_from_status(without_layer);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 100);
    }

    SECTION("missing layer field falls back to estimation") {
        state.set_print_layer_total(200);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json no_layer = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(no_layer);

        // Should estimate: 50% of 200 = 100
        REQUIRE_FALSE(state.has_real_layer_data());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 100);
    }
}

// ============================================================================
// Source precedence: print_stats.info > virtual_sdcard
// ============================================================================
//
// When both sources arrive in the same status update, slicer-supplied data
// (print_stats.info.current_layer / total_layer via SET_PRINT_STATS_INFO)
// must win over the Klipper-side virtual_sdcard fallback. Previously the
// virtual_sdcard branch ran second and silently overwrote the info value —
// preventing slicers that emit accurate per-feature layer numbering from
// being trusted on printers that also report sdcard layers.

TEST_CASE("Layer tracking: print_stats.info wins over virtual_sdcard in same update",
          "[layer_tracking][precedence]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    SECTION("info.current_layer beats virtual_sdcard.layer when both present") {
        json combined = {
            {"print_stats", {{"info", {{"current_layer", 42}, {"total_layer", 200}}}}},
            {"virtual_sdcard", {{"progress", 0.21}, {"layer", 999}, {"layer_count", 9999}}}};
        state.update_from_status(combined);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 42);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 200);
    }

    SECTION("virtual_sdcard takes over when info missing in subsequent update") {
        // First update: info-only — sets layer to 10
        json info_only = {{"print_stats", {{"info", {{"current_layer", 10}, {"total_layer", 100}}}}}};
        state.update_from_status(info_only);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 10);

        // Second update: virtual_sdcard only — should now drive layer
        json sdcard_only = {{"virtual_sdcard", {{"progress", 0.5}, {"layer", 50}, {"layer_count", 100}}}};
        state.update_from_status(sdcard_only);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 50);
    }

    SECTION("partial info — only current_layer set — still prefers info, sdcard fills total") {
        json partial = {{"print_stats", {{"info", {{"current_layer", 7}}}}},
                        {"virtual_sdcard", {{"progress", 0.0}, {"layer", 99}, {"layer_count", 150}}}};
        state.update_from_status(partial);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 7);
        REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 150);
    }
}

// ============================================================================
// Progress-based layer estimation fallback
// ============================================================================

TEST_CASE("Layer tracking: progress-based estimation fallback", "[layer_tracking][estimation]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Start printing
    json printing = {{"print_stats", {{"state", "printing"}}}};
    state.update_from_status(printing);

    // Set total layers from metadata (this is how it works in practice)
    state.set_print_layer_total(320);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("estimates layer from progress when no real layer data") {
        REQUIRE_FALSE(state.has_real_layer_data());

        // 50% progress → ~160/320
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);

        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
    }

    SECTION("estimates at low progress") {
        json progress = {{"virtual_sdcard", {{"progress", 0.01}}}};
        state.update_from_status(progress);

        // 1% of 320 = 3.2, rounded = 3. But clamped to min 1.
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) >= 1);
    }

    SECTION("estimates at high progress") {
        json progress = {{"virtual_sdcard", {{"progress", 0.99}}}};
        state.update_from_status(progress);

        // 99% of 320 = 316.8 → 317
        int estimated = lv_subject_get_int(state.get_print_layer_current_subject());
        REQUIRE(estimated >= 315);
        REQUIRE(estimated <= 320);
    }

    SECTION("does not estimate when total_layers is 0") {
        state.set_print_layer_total(0);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);

        // Should stay at 0 — no total to estimate from
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }

    SECTION("stops estimating once real data arrives from print_stats.info") {
        // First: estimation active
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
        REQUIRE_FALSE(state.has_real_layer_data());

        // Real data arrives
        json real_layer = {{"print_stats", {{"info", {{"current_layer", 142}}}}}};
        state.update_from_status(real_layer);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 142);
        REQUIRE(state.has_real_layer_data());

        // Further progress updates should NOT overwrite real data
        json progress2 = {{"virtual_sdcard", {{"progress", 0.55}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 142);
    }

    SECTION("stops estimating once real data arrives from gcode fallback") {
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE_FALSE(state.has_real_layer_data());

        // Gcode fallback sets real data
        state.set_print_layer_current(150);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(state.has_real_layer_data());

        // Progress update should NOT overwrite
        json progress2 = {{"virtual_sdcard", {{"progress", 0.55}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 150);
    }

    SECTION("does not estimate in terminal state even without real data") {
        // Set total layers and make some progress
        json progress = {{"virtual_sdcard", {{"progress", 0.50}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);

        // Print completes
        json complete = {{"print_stats", {{"state", "complete"}}}};
        state.update_from_status(complete);

        // Progress update arrives after completion — should NOT change layer
        json progress2 = {{"virtual_sdcard", {{"progress", 0.99}}}};
        state.update_from_status(progress2);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 160);
    }

    SECTION("has_real_layer_data resets on new print") {
        // Get real data
        json real_layer = {{"print_stats", {{"info", {{"current_layer", 42}}}}}};
        state.update_from_status(real_layer);
        REQUIRE(state.has_real_layer_data());

        // Simulate new print starting (state goes to standby then printing)
        json standby = {{"print_stats", {{"state", "standby"}}}};
        state.update_from_status(standby);

        // Reset via the same mechanism as real code
        PrinterStateTestAccess::reset(state);
        state.init_subjects(false);

        json printing2 = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(printing2);

        REQUIRE_FALSE(state.has_real_layer_data());
    }
}

// ============================================================================
// Regression: a layer-reporting printer must NEVER fabricate a layer from
// progress during pre-print.
//
// Root cause (Snapmaker U1 premature print-start completion): the progress
// estimate was gated on the PER-PRINT has_real_layer_data_ flag.
// reset_for_new_print() clears that flag at the start of every print, and
// Moonraker's DELTA status updates omit unchanged fields — so while
// info.current_layer sits at 0 through the entire ~4 min pre-print
// (homing / bed detect / auto-feed / clean / mesh / prime), the omitted-but-
// unchanged 0 is never re-observed and has_real_layer_data_ stays false.
// File progress, however, climbs to ~2% during the prime line, so the estimate
// fabricated current_layer = max(1, round(0.02 * total)) = 1. That fake "layer
// 1" tripped MoonrakerManager::should_complete_preprint()'s 0->1 edge ~24 s in,
// ending "Preparing..." long before the real first model layer.
//
// Fix: gate the estimate on the STICKY printer_reports_layers_ capability flag
// instead. The U1 sends total_layer in print_stats.info at print start, so the
// sticky flag latches immediately and the estimate is suppressed for the whole
// session — current_layer holds the authoritative info value (0 through
// pre-print) and only a genuine info.current_layer = 1 advances it.
// ============================================================================

TEST_CASE("Layer tracking: layer-reporting printer never estimates during preprint",
          "[layer_tracking][estimation][snapmaker]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // --- Print starts. The U1 emits SET_PRINT_STATS_INFO TOTAL_LAYER=10 /
    //     CURRENT_LAYER=0 in the slicer header, so info carries both fields up
    //     front. This latches the sticky printer_reports_layers_ capability and
    //     seeds current_layer at the authoritative 0. ---
    json start = {{"print_stats",
                   {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 0}}}}}};
    state.update_from_status(start);

    REQUIRE(state.printer_reports_layers());
    REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.get_print_layer_total_subject()) == 10);

    // --- reset_for_new_print() runs (async, after the collector starts). It
    //     clears the PER-PRINT has_real_layer_data_ flag but NOT the sticky
    //     capability flag. This is the exact window that used to break. ---
    state.reset_for_new_print();
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE_FALSE(state.has_real_layer_data());
    REQUIRE(state.printer_reports_layers()); // sticky — survives reset
    // Re-seed total (reset cleared current to 0; total survives in the subject
    // but re-send it the way Moonraker would on the next delta that carries it).
    json total_only = {{"print_stats", {{"info", {{"total_layer", 10}}}}}};
    state.update_from_status(total_only);

    SECTION("progress climbing during preprint does NOT fabricate layer 1") {
        // Pre-print: file progress creeps up (prime line, ~2%) while
        // info.current_layer is omitted by Moonraker (unchanged 0 → delta drops
        // it). Before the fix this estimated max(1, round(0.02*10)) = 1.
        json progress2pct = {{"virtual_sdcard", {{"progress", 0.02}}}};
        state.update_from_status(progress2pct);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);

        // Progress keeps climbing through the rest of the silent pre-print —
        // still no estimate, layer stays pinned at the authoritative 0.
        json progress15pct = {{"virtual_sdcard", {{"progress", 0.15}}}};
        state.update_from_status(progress15pct);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);
    }

    SECTION("only the real info.current_layer=1 advances the layer") {
        // Pre-print progress — no estimate.
        json progress = {{"virtual_sdcard", {{"progress", 0.05}}}};
        state.update_from_status(progress);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 0);

        // The genuine first model layer: slicer emits CURRENT_LAYER=1 → info.
        json real_layer1 = {{"print_stats", {{"info", {{"current_layer", 1}}}}}};
        state.update_from_status(real_layer1);
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 1);
        REQUIRE(state.has_real_layer_data());
    }
}

// ============================================================================
// Sticky printer_reports_layers — survives per-print reset (FIX C, L093)
// ============================================================================
// Device-grounded regression: on the Snapmaker U1 the printer reports layers
// (print_stats.info.total_layer is present from print start), but
// reset_for_new_print() — which runs when a new print transitions IDLE ->
// preparing — clears the PER-PRINT has_real_layer_data_ flag. The U1 does not
// continuously re-emit info.current_layer during pre-print, so has_real_layer_data
// stays FALSE through the whole purge. The earlier pre-print completion gate
// discriminated on that racy per-print flag and therefore took the print_duration
// fallback mid-purge, completing Preparing minutes early. The STICKY
// printer_reports_layers flag must survive reset_for_new_print() so the gate keeps
// taking the real-first-layer path. This test drives the ACTUAL print_stats parse
// path (not the pure helper with hand-picked args), replicating the device sequence.

TEST_CASE("Layer tracking: printer_reports_layers is sticky across reset_for_new_print",
          "[layer_tracking][print_stats][regression]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Sentinel: a fresh session has never seen layer data.
    REQUIRE_FALSE(state.printer_reports_layers());
    REQUIRE_FALSE(state.has_real_layer_data());

    // --- Print A: U1 reports total_layer at print start (current_layer arrives later). ---
    json print_a_start = {
        {"print_stats", {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 0}}}}}};
    state.update_from_status(print_a_start);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    REQUIRE(state.printer_reports_layers()); // latched immediately from total_layer
    REQUIRE(state.has_real_layer_data());

    // Print A advances and finishes.
    state.update_from_status(
        {{"print_stats", {{"state", "printing"}, {"info", {{"total_layer", 10}, {"current_layer", 10}}}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    state.update_from_status({{"print_stats", {{"state", "complete"}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    // Force phase back to IDLE so the next set_print_start_state triggers the
    // IDLE -> preparing new-print path (which calls reset_for_new_print()).
    state.reset_print_start_state();
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // --- Print B begins: pre-print phase opens. This is the IDLE -> INITIALIZING
    //     transition that fires reset_for_new_print() in set_print_start_state(). ---
    lv_subject_set_int(state.get_print_active_subject(), 1);
    state.set_print_start_state(PrintStartPhase::INITIALIZING, "Preparing Print...", 0);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    SECTION("reset_for_new_print clears the per-print flag but NOT the sticky one") {
        // Per-print flag cleared (the U1 hasn't re-emitted current_layer yet)...
        REQUIRE_FALSE(state.has_real_layer_data());
        // ...but the sticky printer-capability flag survives.
        REQUIRE(state.printer_reports_layers());
    }

    SECTION("Pre-print purge does NOT complete despite has_real_layer_data being false") {
        // EXACT device state mid-purge: per-print flag false, current_layer 0,
        // print_duration ticking up from auto-feed/purge. Discriminating on the
        // sticky flag keeps us on the real-first-layer path → no completion.
        REQUIRE_FALSE(state.has_real_layer_data());
        REQUIRE(state.printer_reports_layers());
        REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/state.printer_reports_layers(),
            /*current_layer=*/lv_subject_get_int(state.get_print_layer_current_subject()),
            /*print_duration=*/120, /*seen_layer_zero=*/true));
    }

    SECTION("Real first layer 0->1 DOES complete once the U1 re-emits current_layer") {
        // U1 emits current_layer=1 at the real first layer.
        state.update_from_status(
            {{"print_stats", {{"state", "printing"}, {"info", {{"current_layer", 1}}}}}});
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.get_print_layer_current_subject()) == 1);
        REQUIRE(state.printer_reports_layers());
        REQUIRE(MoonrakerManager::should_complete_preprint(
            /*printer_reports_layers=*/state.printer_reports_layers(),
            /*current_layer=*/lv_subject_get_int(state.get_print_layer_current_subject()),
            /*print_duration=*/120, /*seen_layer_zero=*/true));
    }
}

TEST_CASE("Layer tracking: never-reporting printer keeps sticky false (fallback preserved)",
          "[layer_tracking][print_stats][regression]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // A genuine non-reporter: state updates arrive with NO layer field anywhere.
    state.update_from_status({{"print_stats", {{"state", "printing"}}}});
    state.update_from_status({{"virtual_sdcard", {{"progress", 0.10}}}});
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE_FALSE(state.printer_reports_layers());

    // With the sticky flag false, the print_duration fallback is preserved so
    // the printer still leaves Preparing on first extrusion.
    REQUIRE(MoonrakerManager::should_complete_preprint(
        /*printer_reports_layers=*/state.printer_reports_layers(),
        /*current_layer=*/0, /*print_duration=*/5, /*seen_layer_zero=*/false));
}

// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// End-to-end lock-in for the headline L0 behavior: a paused printer plus an
// uncoded jam `!!` line must surface a BLOCKING MODAL whose message carries the
// FULL untruncated jam text (acceptance criteria E1 = modal-not-toast, E2 =
// full text). The pure decision logic (error_classify::classify +
// decide_presentation) is covered by test_error_classify.cpp /
// test_gcode_error_routing.cpp; this test closes the gap on the *presentation
// glue* that process_line drives.
//
// IMPORTANT — test-binary boundary (why this test is shaped the way it is):
// The test build EXCLUDES ui_notification.o and replaces ui_notification_error()
// with a logging-only stub in tests/ui_test_utils.cpp (see mk/tests.mk:
// "ui_notification.o ... ui_test_utils.o provides stub ... functions").
// Reason: the real ui_notification.cpp pulls get_notification_subject() from
// app_globals.o, which is itself stubbed in tests — linking both duplicates
// symbols. Consequently process_line()'s PresentAs::MODAL arm calls the STUB,
// which never builds a modal, so an assert on modal_get_top() after
// process_line() ALWAYS fails in the test binary regardless of the feature.
//
// What this test therefore verifies — every link the production path traverses
// EXCEPT the stubbed thunk, with no weakened assertion:
//   1. Routing seam (real classifier + real paused PrinterState): the exact
//      classification process_line performs maps the jam to PresentAs::MODAL.
//      This is the severity gate end-to-end — flip the printer to idle and the
//      same line drops below MODAL.
//   2. Presentation seam (real ui_modal.o — NOT stubbed): handing the modal
//      layer the same (title, detail) that process_line passes to
//      ui_notification_error renders a real modal_dialog whose dialog_message
//      holds the FULL untruncated text. This is the E1+E2 proof against the
//      real widget tree.
//
// Tagged [.ui_integration]: needs the XML component tree on disk (modal_dialog).

#include "app_globals.h"
#include "error_classify.h"
#include "error_event.h"
#include "gcode_error_router.h"
#include "printer_state.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"

#include "lvgl/lvgl.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// The real AFC/runout chinglish: an uncoded `!!` (no {"code":...}), deliberately
// longer than the 80-byte toast truncation cap so E2 (full text) is meaningful.
const std::string kJamLine =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still detect "
    "filament. Possible filament break or jam at the toolhead. Please clear the jam and "
    "reload filament manually, then resume the print.";

// The detail substring process_line would surface (the `!! ` prefix stripped).
const char* kJamDetail = kJamLine.c_str() + 3;

// Walk the modal subtree for the deepest label containing `needle`.
// modal_dialog.xml names the message label "dialog_message"; the substring walk
// is robust to the text landing on any label inside the scrollable content.
const char* find_text_containing(lv_obj_t* node, const char* needle) {
    if (!node) return nullptr;
    if (lv_obj_check_type(node, &lv_label_class)) {
        const char* txt = lv_label_get_text(node);
        if (txt && std::strstr(txt, needle)) return txt;
    }
    uint32_t n = lv_obj_get_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
        if (const char* hit = find_text_containing(lv_obj_get_child(node, i), needle)) return hit;
    }
    return nullptr;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Uncoded jam while paused surfaces a modal with the full message",
                 "[error-center][routing][e2e][.ui_integration]") {
    // ---- Seam 1: routing decision (REAL classifier, REAL paused state) ----
    // Mark the singleton printer paused — process_line reads
    // get_printer_state().is_paused() into ClassifyContext, which makes an
    // uncoded `!!` CRITICAL, which decide_presentation maps to MODAL.
    get_printer_state().update_from_status(
        nlohmann::json{{"pause_resume", {{"is_paused", true}}}});
    REQUIRE(get_printer_state().is_paused());

    // Reproduce process_line's classification path verbatim: same context
    // construction, same classify() call. This is the routing process_line
    // performs before it reaches the PresentAs::MODAL arm.
    helix::ClassifyContext ctx;
    ctx.is_paused = get_printer_state().is_paused();
    ctx.is_printing =
        get_printer_state().get_print_job_state() == helix::PrintJobState::PRINTING;

    auto ev = helix::error_classify::classify(kJamLine, ctx);
    REQUIRE(ev.has_value());
    REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
    // E1: the routing decision is a blocking modal, not a toast.
    REQUIRE(helix::decide_presentation(*ev) == helix::PresentAs::MODAL);
    // The detail process_line would surface is the full line, untruncated.
    REQUIRE(ev->detail == kJamDetail);

    // Severity gate sanity: an idle (not paused, not printing) printer demotes
    // the same line below MODAL — proving the gate, not a constant.
    {
        helix::ClassifyContext idle;
        idle.is_paused = false;
        idle.is_printing = false;
        auto idle_ev = helix::error_classify::classify(kJamLine, idle);
        REQUIRE(idle_ev.has_value());
        REQUIRE(helix::decide_presentation(*idle_ev) != helix::PresentAs::MODAL);
    }

    // ---- Drive the REAL glue (process_line) ----
    // Exercise the actual private presentation glue via the friend accessor:
    // real GcodeErrorRouter, real classifier, real paused PrinterState. This
    // runs modal_title_for(), the rpc-correlation dedup guard, and the switch's
    // PresentAs::MODAL arm. Its terminal ui_notification_error() is a logging
    // stub in the test binary (see header), so it cannot render the modal here
    // — but it MUST classify + route without crashing and reach the MODAL arm.
    // The visible-modal proof is Seam 2 below, against the real modal layer.
    helix::GcodeErrorRouter router(nullptr, nullptr);
    REQUIRE_NOTHROW(GcodeErrorRouterTestAccess::process_line(router, kJamLine));
    helix::ui::UpdateQueue::instance().drain();

    // ---- Seam 2: presentation (REAL ui_modal.o) ----
    // process_line's MODAL arm calls
    //   ui_notification_error(modal_title_for(*ev), ev->detail.c_str(), /*modal=*/true)
    // which, in production, routes the modal case to modal_show_alert(...). In
    // the test binary ui_notification_error is a logging-only stub (see header
    // comment), so we drive the same modal layer it would with the SAME
    // (title, detail) process_line hands it. modal_title_for(GENERIC) ==
    // "Printer Error".
    lv_obj_t* dialog = helix::ui::modal_show_alert("Printer Error", ev->detail.c_str(),
                                                   ModalSeverity::Error, "OK");
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(50);

    // E1: a blocking modal is visible.
    REQUIRE(dialog != nullptr);
    lv_obj_t* modal = helix::ui::modal_get_top();
    REQUIRE(modal != nullptr);
    REQUIRE(Modal::any_visible());

    // E2: the modal carries the FULL, untruncated jam text. The asserted
    // substrings sit well past the 80-byte toast cap, so their presence proves
    // the text was not truncated for a toast.
    const char* msg = find_text_containing(modal, "reload filament manually");
    REQUIRE(msg != nullptr);
    REQUIRE(std::string(msg).find("resume the print") != std::string::npos);

    Modal::hide(dialog);
    process_lvgl(20);
}

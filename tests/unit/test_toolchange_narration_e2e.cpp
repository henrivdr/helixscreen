// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// End-to-end integration coverage for toolchange narration -> step display.
//
// Drives a realistic AFC swap narration sequence through the REAL
// GcodeNarrationRouter (both entry points: process_line AND the
// Moonraker-envelope parser on_notify_gcode_response) and asserts:
//   S1 - the "// Purge" line advances to the dedicated "purge" phase (index 5),
//        NOT the "feed" phase (index 4); ams_action_detail reads "Purge".
//   S2 - the LOAD_SWAP step template the sidebar renders includes the
//        "Brush nozzle" and "Clean nozzle" phases.
//
// The router unit test (test_gcode_narration_router.cpp) covers process_line
// in isolation; on_notify_gcode_response was previously UNTESTED. This file
// exercises both, plus the template-derived step labels the sidebar consumes.

#include "ui_step_progress.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "gcode_narration_router.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp" // libhv-bundled nlohmann::json, same header the router uses

using namespace helix;

// Test-only friend of GcodeNarrationRouter (declared in
// gcode_narration_router.h as `friend struct ::GcodeNarrationRouterTestAccess`).
// Exposes BOTH private entry points. This definition MUST stay token-identical
// to the one in test_gcode_narration_router.cpp (same struct, global namespace)
// to avoid an ODR violation across translation units.
struct GcodeNarrationRouterTestAccess {
    static void feed(GcodeNarrationRouter& r, const std::string& line) {
        r.process_line(line);
    }
    static void notify(GcodeNarrationRouter& r, const nlohmann::json& msg) {
        r.on_notify_gcode_response(msg);
    }
};

namespace {

// AFC backend constructed with (nullptr, nullptr) — same idiom as
// AfcToolchangeTestHelper in test_afc_toolchange.cpp. AFC overrides
// toolchange_phase_template() + match_narration_phase(), which is exactly the
// step model the router needs to resolve a `//` narration line to an index.
void install_afc_backend() {
    AmsState::instance().set_backend(std::make_unique<AmsBackendAfc>(nullptr, nullptr));
}

// AmsState is a process-wide singleton; init_subjects is a no-op once
// initialized so toolchange_step persists between cases. Establish a known
// LOAD_SWAP + sentinel(-1) baseline so each test is order-independent.
void reset_step_baseline() {
    AmsState::instance().init_subjects(true);
    AmsState::instance().set_active_step_operation(StepOperationType::LOAD_SWAP);
    AmsState::instance().set_narration_phase(-1, "");
    install_afc_backend();
}

int current_step() {
    return lv_subject_get_int(AmsState::instance().get_toolchange_step_subject());
}

std::string current_detail() {
    const char* s = lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject());
    return s ? std::string(s) : std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Router advances the subject through a realistic swap sequence (S1 core).
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - realistic swap sequence advances steps",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr); // null client => no real subscription

    // LOAD_SWAP template indices:
    //   heat=0, cut=1, poop=2, kick=3, feed=4, purge=5, brush=6, clean=7, load=8
    struct Step {
        const char* line;
        int expected_index;
        const char* expected_detail;
    };
    const Step sequence[] = {
        {"// Heat nozzle", 0, "Heat nozzle"},
        {"// Cutting tip", 1, "Cut tip"},
        {"// Feed filament", 4, "Feed filament"},
        {"// Purge", 5, "Purge"}, // S1: NOT 4 ("Feed filament")
        {"// Move to Brush", 6, "Brush nozzle"},
        {"// AFC_Brush: Clean Nozzle", 7, "Clean nozzle"},
    };

    for (const auto& step : sequence) {
        GcodeNarrationRouterTestAccess::feed(router, step.line);

        INFO("line: " << step.line);

        // set_narration_phase writes both subjects synchronously on the main
        // thread (feed() invokes process_line directly), so the label is
        // observable immediately after feed().
        REQUIRE(current_detail() == std::string(step.expected_detail));

        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(current_step() == step.expected_index);
        // Narration is now the single top-priority writer of ams_action_detail
        // (recompute_action_detail reads last_narration_label_ before anything
        // else), so the precise phase label SURVIVES a drain — the print-state
        // observer's recompute can no longer clobber it back to "Idle".
        REQUIRE(current_detail() == std::string(step.expected_detail));
    }

    // S1 acceptance, stated explicitly: "// Purge" resolves to the dedicated
    // purge phase (index 5 / "Purge"), NEVER the preceding "Feed filament"
    // phase (index 4). Re-feed in isolation to make the invariant unmistakable.
    GcodeNarrationRouterTestAccess::feed(router, "// Purging filament");
    REQUIRE(current_detail() == std::string("Purge"));
    REQUIRE(current_detail() != std::string("Feed filament"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 5);
    // Single-writer guarantee: the precise phase label is stable post-drain.
    REQUIRE(current_detail() == std::string("Purge"));
}

// ---------------------------------------------------------------------------
// 2. on_notify_gcode_response Moonraker-envelope parsing (closes untested gap).
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - on_notify_gcode_response parses Moonraker envelope",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    GcodeNarrationRouter router(nullptr, nullptr);

    // Flat params form: {"params": ["// AFC_Brush: Clean Nozzle"]}
    nlohmann::json flat = {{"params", nlohmann::json::array({"// AFC_Brush: Clean Nozzle"})}};
    GcodeNarrationRouterTestAccess::notify(router, flat);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 7); // clean

    // Reset, then the nested params form: {"params": [["// Purge"]]}
    AmsState::instance().set_narration_phase(-1, "");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == -1);

    nlohmann::json nested = {
        {"params", nlohmann::json::array({nlohmann::json::array({"// Purge"})})}};
    GcodeNarrationRouterTestAccess::notify(router, nested);
    REQUIRE(current_detail() == std::string("Purge"));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(current_step() == 5); // S1: purge, not feed (4)
    // Single-writer guarantee: the phase label survives the drain.
    REQUIRE(current_detail() == std::string("Purge"));
}

// ---------------------------------------------------------------------------
// 3. Template-derived labels (the data the sidebar renders) -- S2 + S1.
// ---------------------------------------------------------------------------
TEST_CASE_METHOD(LVGLTestFixture,
                 "Toolchange narration E2E - LOAD_SWAP step template carries brush/clean/purge",
                 "[narration][ui_integration]") {
    reset_step_baseline();
    auto* backend = dynamic_cast<AmsBackendAfc*>(AmsState::instance().get_backend());
    REQUIRE(backend != nullptr);

    const auto tmpl = backend->toolchange_phase_template(StepOperationType::LOAD_SWAP);

    auto label_at = [&](const std::string& id) -> std::string {
        for (const auto& p : tmpl) {
            if (p.id == id)
                return p.label;
        }
        return {};
    };
    auto index_of = [&](const std::string& id) -> int {
        for (size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i].id == id)
                return static_cast<int>(i);
        }
        return -1;
    };
    auto has_label = [&](const std::string& want) {
        for (const auto& p : tmpl) {
            if (p.label == want)
                return true;
        }
        return false;
    };

    // S2: the swap template the sidebar renders must include brush + clean.
    REQUIRE(has_label("Brush nozzle"));
    REQUIRE(has_label("Clean nozzle"));

    // S1: the phase "// Purge" resolves to is labeled "Purge" (its own phase),
    // and sits AFTER "feed" so it is never conflated with "Feed filament".
    const int purge_idx = index_of("purge");
    const int feed_idx = index_of("feed");
    REQUIRE(purge_idx == 5);
    REQUIRE(feed_idx == 4);
    REQUIRE(purge_idx > feed_idx);
    REQUIRE(label_at("purge") == std::string("Purge"));

    // Build the step bar exactly as the sidebar does, from the same template,
    // and confirm the rendered widget mirrors the template labels. This is the
    // real label source the AmsOperationSidebar feeds ui_step_progress_create.
    std::vector<ui_step_t> steps;
    steps.reserve(tmpl.size());
    for (const auto& p : tmpl) {
        steps.push_back({p.label.c_str(), helix::StepState::Pending});
    }
    lv_obj_t* bar =
        ui_step_progress_create(lv_screen_active(), steps.data(), static_cast<int>(steps.size()),
                                /*horizontal=*/true, /*scope_name=*/nullptr);
    REQUIRE(bar != nullptr);
    // ui_step_progress was created from the same labels; nothing further to
    // assert beyond construction succeeding (its child structure moves
    // connectors around, so the template is the authoritative label source).

    lv_obj_delete(bar);
}

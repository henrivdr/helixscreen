// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Wizard step-transition stress harness.
//
// Repeatedly drives ui_wizard_navigate_to_step() between steps 2 (WiFi) and
// 3 (Connection) to surface the chronic heap corruption family that has
// landed five separate fixes (#793, #827, #840, #871, #880, plus tonight's
// SIGABRT in lv_draw_sw_blend_image during step 3->2 back-nav).
//
// Run under AddressSanitizer for actionable UAF reports:
//   make test SANITIZE=address
//   ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:fast_unwind_on_malloc=0 \
//       ./build/bin/helix-tests "[stress][wizard]"
//
// Default iteration count is moderate; override with WIZARD_STRESS_ITERATIONS
// in the environment for longer soak runs.
//
// The test is tagged [.ui_integration] (hidden by default) because it needs
// the XML component tree on disk. Run explicitly via the [stress] tag.

#include "ui_update_queue.h"
#include "ui_wizard.h"

#include "../lvgl_ui_test_fixture.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int env_iterations(int default_count) {
    if (const char* v = std::getenv("WIZARD_STRESS_ITERATIONS")) {
        try {
            int n = std::stoi(v);
            if (n > 0)
                return n;
        } catch (...) {
        }
    }
    return default_count;
}

class WizardStressFixture : public LVGLUITestFixture {
  public:
    WizardStressFixture() {
        wizard_ = ui_wizard_create(test_screen());
        if (!wizard_) {
            spdlog::error("[WizardStressFixture] ui_wizard_create returned null");
            return;
        }
        // Without the XML component tree on disk this test can't drive the
        // real navigation paths. Skip rather than spuriously pass.
        ready_ = (lv_obj_find_by_name(wizard_, "wizard_content") != nullptr);
    }

    ~WizardStressFixture() {
        wizard_ = nullptr;
    }

    void require_ready() {
        if (!ready_)
            SKIP("Wizard XML components not available");
    }

    lv_obj_t* wizard_ = nullptr;
    bool ready_ = false;
};

} // namespace

TEST_CASE_METHOD(WizardStressFixture, "Wizard stress: bounce step 2 <-> 3",
                 "[wizard][stress][.ui_integration]") {
    require_ready();

    const int iterations = env_iterations(50);
    spdlog::info("[wizard-stress] bouncing 2<->3 for {} iterations", iterations);

    // Settle one full nav before the loop so we start from a known state.
    ui_wizard_navigate_to_step(helix::wizard::StepId::Wifi);
    process_lvgl(120);

    for (int i = 0; i < iterations; ++i) {
        ui_wizard_navigate_to_step(helix::wizard::StepId::Connection);
        // Long enough to let the slide-out animation start, the async delete
        // pipeline drain, and any observer-driven rebuild fire.
        process_lvgl(80);

        ui_wizard_navigate_to_step(helix::wizard::StepId::Wifi);
        process_lvgl(80);

        if ((i + 1) % 10 == 0) {
            spdlog::info("[wizard-stress] iteration {}/{}", i + 1, iterations);
        }
    }

    // If we reach here without the process aborting, ASAN didn't trip on the
    // bounce path. That doesn't mean the code is clean — only that this
    // specific traversal didn't hit a freed pointer.
    SUCCEED("Completed " << iterations << " bounces without crash");
}

TEST_CASE_METHOD(WizardStressFixture, "Wizard stress: full sweep 1..N",
                 "[wizard][stress][.ui_integration]") {
    require_ready();

    // Probe how many steps the wizard exposes by walking forward until
    // navigate_to_step refuses (current_step subject won't update).
    // Wizards historically have 7 steps but we don't hard-code that.
    constexpr int max_probe = 12;
    int last_valid = 1;
    for (int s = 1; s <= max_probe; ++s) {
        ui_wizard_navigate_to_step(static_cast<helix::wizard::StepId>(s));
        process_lvgl(40);
        if (lv_obj_find_by_name(wizard_, "wizard_content") == nullptr)
            break;
        last_valid = s;
    }
    spdlog::info("[wizard-stress] sweep range: 1..{}", last_valid);

    const int iterations = env_iterations(20);
    for (int i = 0; i < iterations; ++i) {
        for (int s = 1; s <= last_valid; ++s) {
            ui_wizard_navigate_to_step(static_cast<helix::wizard::StepId>(s));
            process_lvgl(60);
        }
        for (int s = last_valid; s >= 1; --s) {
            ui_wizard_navigate_to_step(static_cast<helix::wizard::StepId>(s));
            process_lvgl(60);
        }
    }

    SUCCEED("Completed " << iterations << " full sweeps without crash");
}

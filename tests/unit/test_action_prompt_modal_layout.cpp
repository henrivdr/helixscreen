// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout regression tests for ActionPromptModal's regular-button row.
//
// R2 / prestonbrown/helixscreen#1043: the Klipper action:prompt modal lays its
// regular (non-footer) buttons into a fixed-width (320px) "button_container"
// with flex_flow=row_wrap and content-sized buttons. AFC's lane-picker prompt
// supplies 4 lane buttons ("Lane 1".."Lane 4"); they overflow the ~288px usable
// width and the 4th wraps to a second line.
//
// Fix: when there are >= 4 regular buttons, lay them out as an equal-width
// non-wrapping row (flex grow=1) so they all fit on ONE line. With <= 3 regular
// buttons the legacy content-sized row_wrap behaviour is kept byte-for-byte.
//
// Tagged [ui_integration] (NOT hidden) — it shows real widgets and forces a
// real LVGL layout pass, which is the only way the wrap actually manifests.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"

#include <vector>

#include "../catch_amalgamated.hpp"

// White-box accessor (declared friend in action_prompt_modal.h) — avoids adding
// _for_testing() methods to the production class ([L065]/[L088]).
struct ActionPromptModalTestAccess {
    static const std::vector<lv_obj_t*>& buttons(const helix::ui::ActionPromptModal& m) {
        return m.created_buttons_;
    }
};

namespace {

helix::PromptData make_regular_prompt(int n_buttons) {
    helix::PromptData data;
    data.title = "Select Lane";
    data.text_lines.push_back("Pick a lane to calibrate");
    for (int i = 0; i < n_buttons; ++i) {
        helix::PromptButton btn;
        btn.label = "Lane " + std::to_string(i + 1);
        btn.gcode = "AFC_CALIBRATION LANE=lane" + std::to_string(i + 1);
        btn.color = "primary";
        btn.is_footer = false;
        data.buttons.push_back(std::move(btn));
    }
    return data;
}

class ActionPromptLayoutFixture : public LVGLUITestFixture {
  public:
    ActionPromptLayoutFixture() {
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });
    }
    ~ActionPromptLayoutFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    helix::ui::ActionPromptModal modal_;
    bool prev_animations_ = true;
};

} // namespace

// ----------------------------------------------------------------------------
// 4 regular buttons must share a single row (the R2 bug case).
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: 4 regular buttons fit on one row",
                 "[action_prompt][layout][ui_integration]") {
    auto data = make_regular_prompt(4);
    REQUIRE(modal_.show_prompt(test_screen(), data));

    // Force a synchronous layout pass on the whole tree so geometry settles.
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 4);

    // All four buttons must be on the SAME visual row. If the 4th wraps, its y
    // jumps down by ~one button-height (>= ~48px) — far outside this tolerance.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        int32_t yi = lv_obj_get_y(btns[i]);
        INFO("button[" << i << "] y=" << yi << " vs button[0] y=" << y0);
        REQUIRE(std::abs(yi - y0) <= 4);
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Regression: 3 regular buttons still share one row AND stay content-sized
// (NOT forced to equal width). Guards against regressing the <= 3 path.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: 3 regular buttons stay content-sized on one row",
                 "[action_prompt][layout][ui_integration]") {
    // Short labels of unequal length: they fit on one content-sized row (as the
    // legacy <= 3 path always has), and content-sizing yields unequal widths —
    // the >= 4 equal-width path would instead force them identical.
    helix::PromptData data;
    data.title = "Confirm";
    data.text_lines.push_back("Choose an action");
    data.buttons.push_back({"OK", "RESUME", "primary", "", false, -1});
    data.buttons.push_back({"Retry", "RETRY", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "CANCEL", "error", "", false, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 3);

    // One row: all share (approximately) the same y.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        REQUIRE(std::abs(lv_obj_get_y(btns[i]) - y0) <= 4);
    }

    // Content-sized: the short and long labels must NOT be equal width.
    int32_t w_ok = lv_obj_get_width(btns[0]);
    int32_t w_long = lv_obj_get_width(btns[1]);
    INFO("w(OK)=" << w_ok << " w(Retry the whole operation)=" << w_long);
    REQUIRE(w_long > w_ok);

    modal_.hide();
}

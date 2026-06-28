// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_control_overlay.cpp
 * @brief Unit tests for LedControlOverlay color-picker visibility gating.
 *
 * The quick-control overlay must only show the color swatches + custom-color
 * button when the currently selected NATIVE strip is actually RGB-capable.
 * White-only strips (e.g. Flashforge AD5M `[led chamber_light]` with only a
 * white_pin, supports_color=false) used to show the picker; picking a color
 * silently converted RGB->white luminance, which is misleading UX.
 *
 * @see ui_led_control_overlay.h
 * @see LedSettingsOverlay::populate_auto_state_rows() (the mirrored capability gate)
 */

#include "../lvgl_test_fixture.h"
#include "led/led_backend.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::led;

// Test access to exercise the private color-picker visibility gate. Keeps
// test-only entry points out of the production class (L065). Constructs an
// overlay, drives selected_backend_type_ + update_section_visibility(), and
// reads back the color_visible_ subject.
//
// Must live in namespace helix::led to match the `friend class
// LedControlOverlayTestAccess;` declaration inside that namespace.
namespace helix::led {
class LedControlOverlayTestAccess {
  public:
    explicit LedControlOverlayTestAccess(helix::PrinterState& ps) : overlay_(ps) {
        overlay_.init_subjects();
    }
    // overlay_.subjects_ (SubjectManager) deinits its LVGL subjects in its own
    // destructor — no explicit teardown needed here.

    void set_backend(LedBackendType type) {
        overlay_.selected_backend_type_ = type;
    }

    int compute_color_visible() {
        overlay_.update_section_visibility();
        return lv_subject_get_int(&overlay_.color_visible_);
    }

  private:
    helix::led::LedControlOverlay overlay_;
};
} // namespace helix::led

using helix::led::LedControlOverlayTestAccess;

namespace {

// Build a native strip with the requested color capability.
LedStripInfo make_native_strip(const std::string& id, bool supports_color, bool supports_white) {
    LedStripInfo strip;
    strip.name = id;
    strip.id = id;
    strip.backend = LedBackendType::NATIVE;
    strip.supports_color = supports_color;
    strip.supports_white = supports_white;
    return strip;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker hidden for white-only native strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // White-only strip: e.g. AD5M [led chamber_light] with only white_pin.
    ctrl.native().add_strip(make_native_strip("led chamber_light", /*color=*/false,
                                              /*white=*/true));
    ctrl.set_selected_strips({"led chamber_light"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    REQUIRE(access.compute_color_visible() == 0);

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker shown for RGB-capable native strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // RGB strip: e.g. neopixel chamber_light.
    ctrl.native().add_strip(make_native_strip("neopixel chamber_light", /*color=*/true,
                                              /*white=*/true));
    ctrl.set_selected_strips({"neopixel chamber_light"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    REQUIRE(access.compute_color_visible() == 1);

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker hidden when both RGB and white-only selected "
                 "resolves on any color-capable strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("led white_only", /*color=*/false, /*white=*/true));
    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    // Only the white-only strip selected -> hidden.
    ctrl.set_selected_strips({"led white_only"});
    REQUIRE(access.compute_color_visible() == 0);

    // Selection includes a color-capable strip -> shown.
    ctrl.set_selected_strips({"led white_only", "neopixel rgb"});
    REQUIRE(access.compute_color_visible() == 1);

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "LedControlOverlay: color picker hidden for output_pin backend",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // An output_pin LED is brightness-only; the color section must stay hidden
    // regardless of any native strips that happen to exist.
    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));
    ctrl.set_selected_strips({"neopixel rgb"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::OUTPUT_PIN);

    REQUIRE(access.compute_color_visible() == 0);

    ctrl.deinit();
}

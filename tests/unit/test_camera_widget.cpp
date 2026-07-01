// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl.h"
#include "panel_widget.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

#if HELIX_HAS_CAMERA
#include "camera_stream.h"
#endif

using namespace helix;

// ============================================================================
// Registry: camera widget definition
// ============================================================================

#if HELIX_HAS_CAMERA

TEST_CASE("CameraWidget: registered in widget registry", "[camera][panel_widget]") {
    const auto* def = find_widget_def("camera");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Camera");
    REQUIRE(std::string(def->icon) == "video");
    REQUIRE(def->hardware_gate_subject == nullptr);
    REQUIRE(def->default_enabled == false); // opt-in widget
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);
    REQUIRE(def->min_colspan == 1);
    REQUIRE(def->min_rowspan == 1);
    REQUIRE(def->max_colspan == 4);
    REQUIRE(def->max_rowspan == 3);
}

// ============================================================================
// MJPEG boundary parser
// ============================================================================

TEST_CASE("CameraStream: construct and destroy without crash", "[camera]") {
    CameraStream stream;
    REQUIRE_FALSE(stream.is_running());
}

TEST_CASE("CameraStream: start with empty URLs does nothing", "[camera]") {
    CameraStream stream;
    bool callback_called = false;
    stream.start("", "", [&](lv_draw_buf_t*) { callback_called = true; });
    REQUIRE_FALSE(stream.is_running());
    REQUIRE_FALSE(callback_called);
}

TEST_CASE("CameraStream: compute_scaled_size picks smallest sufficient scale", "[camera]") {
    CameraStream stream;

    SECTION("no target set returns source dimensions") {
        auto s = stream.compute_scaled_size(1920, 1080);
        CHECK(s.w == 1920);
        CHECK(s.h == 1080);
    }

    SECTION("source already at or below target returns source") {
        stream.set_target_size(1920, 1080);
        auto s = stream.compute_scaled_size(800, 480);
        CHECK(s.w == 800);
        CHECK(s.h == 480);
    }

    // Remaining scaling factor tests require libturbojpeg at runtime.
    // compute_scaled_size gracefully returns source dims if unavailable.
}

// ============================================================================
// Pixel copy: RGB → LVGL BGR swap
// ============================================================================

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl swaps R and B channels", "[camera]") {
    // 2x2 image: known RGB values
    // Pixel (0,0) = R=0xFF G=0x00 B=0x00 (pure red)
    // Pixel (1,0) = R=0x00 G=0xFF B=0x00 (pure green)
    // Pixel (0,1) = R=0x00 G=0x00 B=0xFF (pure blue)
    // Pixel (1,1) = R=0x12 G=0x34 B=0x56
    const int W = 2, H = 2;
    const int stride = W * 3;
    // clang-format off
    uint8_t src[12] = {
        0xFF, 0x00, 0x00,   0x00, 0xFF, 0x00,  // row 0: red, green
        0x00, 0x00, 0xFF,   0x12, 0x34, 0x56,  // row 1: blue, mixed
    };
    // clang-format on
    uint8_t dst[12] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, false, false);

    // After swap: LVGL stores B,G,R
    // Pixel (0,0): was R=FF,G=00,B=00 → stored B=00,G=00,R=FF
    CHECK(dst[0] == 0x00); // B
    CHECK(dst[1] == 0x00); // G
    CHECK(dst[2] == 0xFF); // R

    // Pixel (1,0): was R=00,G=FF,B=00 → stored B=00,G=FF,R=00
    CHECK(dst[3] == 0x00);
    CHECK(dst[4] == 0xFF);
    CHECK(dst[5] == 0x00);

    // Pixel (0,1): was R=00,G=00,B=FF → stored B=FF,G=00,R=00
    CHECK(dst[6] == 0xFF);
    CHECK(dst[7] == 0x00);
    CHECK(dst[8] == 0x00);

    // Pixel (1,1): was R=12,G=34,B=56 → stored B=56,G=34,R=12
    CHECK(dst[9] == 0x56);
    CHECK(dst[10] == 0x34);
    CHECK(dst[11] == 0x12);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with horizontal flip", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    // Row: [R1,G1,B1] [R2,G2,B2] = [0xAA,0xBB,0xCC] [0x11,0x22,0x33]
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, true, false);

    // Flip H: pixel order reversed. Plus RGB→BGR swap.
    // dst[0..2] = src pixel 1 swapped: R=0x11,G=0x22,B=0x33 → B=0x33,G=0x22,R=0x11
    CHECK(dst[0] == 0x33);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x11);
    // dst[3..5] = src pixel 0 swapped: R=0xAA,G=0xBB,B=0xCC → B=0xCC,G=0xBB,R=0xAA
    CHECK(dst[3] == 0xCC);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xAA);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with vertical flip", "[camera]") {
    const int W = 1, H = 2;
    const int stride = W * 3;
    // Row 0: [0xAA,0xBB,0xCC], Row 1: [0x11,0x22,0x33]
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, false, true);

    // Flip V: row order reversed. Plus RGB→BGR swap.
    // dst row 0 = src row 1 swapped: R=0x11,G=0x22,B=0x33 → B=0x33,G=0x22,R=0x11
    CHECK(dst[0] == 0x33);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x11);
    // dst row 1 = src row 0 swapped: R=0xAA,G=0xBB,B=0xCC → B=0xCC,G=0xBB,R=0xAA
    CHECK(dst[3] == 0xCC);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xAA);
}

TEST_CASE("CameraStream: copy_pixels_rgb_to_lvgl with both flips", "[camera]") {
    const int W = 2, H = 2;
    const int stride = W * 3;
    // Row 0: [0xAA,0xBB,0xCC] [0x11,0x22,0x33]
    // Row 1: [0x44,0x55,0x66] [0x77,0x88,0x99]
    uint8_t src[12] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    uint8_t dst[12] = {};

    CameraStream::copy_pixels_rgb_to_lvgl(src, dst, W, H, stride, stride, true, true);

    // Flip H+V = 180° rotation. Source pixel layout:
    //   (0,0)=AA,BB,CC  (1,0)=11,22,33
    //   (0,1)=44,55,66  (1,1)=77,88,99
    // After 180° rotation + RGB→BGR swap:
    //   dst(0,0) = src(1,1) swapped = B=99,G=88,R=77
    //   dst(1,0) = src(0,1) swapped = B=66,G=55,R=44
    //   dst(0,1) = src(1,0) swapped = B=33,G=22,R=11
    //   dst(1,1) = src(0,0) swapped = B=CC,G=BB,R=AA
    CHECK(dst[0] == 0x99);
    CHECK(dst[1] == 0x88);
    CHECK(dst[2] == 0x77);
    CHECK(dst[3] == 0x66);
    CHECK(dst[4] == 0x55);
    CHECK(dst[5] == 0x44);
    CHECK(dst[6] == 0x33);
    CHECK(dst[7] == 0x22);
    CHECK(dst[8] == 0x11);
    CHECK(dst[9] == 0xCC);
    CHECK(dst[10] == 0xBB);
    CHECK(dst[11] == 0xAA);
}

// ============================================================================
// Pixel copy: BGR no-swap paths (turbojpeg output)
// ============================================================================

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR no-swap fast path", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, false, false, false);

    // No swap, no flip: straight memcpy
    CHECK(dst[0] == 0xAA);
    CHECK(dst[1] == 0xBB);
    CHECK(dst[2] == 0xCC);
    CHECK(dst[3] == 0x11);
    CHECK(dst[4] == 0x22);
    CHECK(dst[5] == 0x33);
}

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR with horizontal flip", "[camera]") {
    const int W = 2, H = 1;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, true, false, false);

    // Flip H only, no swap: pixel order reversed, channels preserved
    CHECK(dst[0] == 0x11);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x33);
    CHECK(dst[3] == 0xAA);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xCC);
}

TEST_CASE("CameraStream: copy_pixels_to_lvgl BGR with vertical flip", "[camera]") {
    const int W = 1, H = 2;
    const int stride = W * 3;
    uint8_t src[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
    uint8_t dst[6] = {};

    CameraStream::copy_pixels_to_lvgl(src, dst, W, H, stride, stride, false, true, false);

    // Flip V only, no swap: row order reversed, channels preserved
    CHECK(dst[0] == 0x11);
    CHECK(dst[1] == 0x22);
    CHECK(dst[2] == 0x33);
    CHECK(dst[3] == 0xAA);
    CHECK(dst[4] == 0xBB);
    CHECK(dst[5] == 0xCC);
}

// ============================================================================
// Boundary parsing from Content-Type
// ============================================================================

TEST_CASE("CameraStream: parse_boundary extracts boundary from Content-Type", "[camera]") {
    SECTION("standard boundary") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace;boundary=myboundary");
        CHECK(b == "--myboundary");
    }
    SECTION("boundary with space after semicolon") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace; boundary=frame");
        CHECK(b == "--frame");
    }
    SECTION("quoted boundary") {
        auto b =
            CameraStream::parse_boundary("multipart/x-mixed-replace; boundary=\"someboundary\"");
        CHECK(b == "--someboundary");
    }
    SECTION("boundary already has dashes") {
        auto b = CameraStream::parse_boundary("multipart/x-mixed-replace;boundary=--existing");
        CHECK(b == "--existing");
    }
    SECTION("no boundary parameter") {
        auto b = CameraStream::parse_boundary("image/jpeg");
        CHECK(b.empty());
    }
    SECTION("empty string") {
        auto b = CameraStream::parse_boundary("");
        CHECK(b.empty());
    }
}

#else // !HELIX_HAS_CAMERA

TEST_CASE("CameraWidget: not registered on embedded platforms", "[camera][panel_widget]") {
    const auto* def = find_widget_def("camera");
    REQUIRE(def == nullptr);
}

#endif // HELIX_HAS_CAMERA

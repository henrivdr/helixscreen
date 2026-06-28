// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_bitmap.h"

#include "../catch_amalgamated.hpp"

using helix::LabelBitmap;

TEST_CASE("LabelBitmap::create initializes correctly", "[label]") {
    auto bmp = LabelBitmap::create(100, 50);
    REQUIRE(bmp.width() == 100);
    REQUIRE(bmp.height() == 50);
    REQUIRE_FALSE(bmp.empty());

    // All pixels should be white (0)
    for (int y = 0; y < 50; y++) {
        for (int x = 0; x < 100; x++) {
            REQUIRE_FALSE(bmp.get_pixel(x, y));
        }
    }
}

TEST_CASE("LabelBitmap::row_byte_width", "[label]") {
    REQUIRE(LabelBitmap::create(1, 1).row_byte_width() == 1);
    REQUIRE(LabelBitmap::create(8, 1).row_byte_width() == 1);
    REQUIRE(LabelBitmap::create(9, 1).row_byte_width() == 2);
    REQUIRE(LabelBitmap::create(16, 1).row_byte_width() == 2);
    REQUIRE(LabelBitmap::create(17, 1).row_byte_width() == 3);
    REQUIRE(LabelBitmap::create(306, 1).row_byte_width() == 39);
}

TEST_CASE("LabelBitmap::set_pixel/get_pixel round-trip", "[label]") {
    auto bmp = LabelBitmap::create(32, 16);

    SECTION("single pixel") {
        bmp.set_pixel(0, 0, true);
        REQUIRE(bmp.get_pixel(0, 0));
        REQUIRE_FALSE(bmp.get_pixel(1, 0));
    }

    SECTION("MSB-first packing") {
        // Pixel 0 should be bit 7 of byte 0
        bmp.set_pixel(0, 0, true);
        REQUIRE((bmp.row_data(0)[0] & 0x80) != 0);

        // Pixel 7 should be bit 0 of byte 0
        bmp.set_pixel(7, 0, true);
        REQUIRE((bmp.row_data(0)[0] & 0x01) != 0);

        // Pixel 8 should be bit 7 of byte 1
        bmp.set_pixel(8, 0, true);
        REQUIRE((bmp.row_data(0)[1] & 0x80) != 0);
    }

    SECTION("edge pixels") {
        bmp.set_pixel(31, 15, true);
        REQUIRE(bmp.get_pixel(31, 15));
        REQUIRE_FALSE(bmp.get_pixel(30, 15));
    }

    SECTION("clear pixel") {
        bmp.set_pixel(5, 5, true);
        REQUIRE(bmp.get_pixel(5, 5));
        bmp.set_pixel(5, 5, false);
        REQUIRE_FALSE(bmp.get_pixel(5, 5));
    }

    SECTION("out-of-bounds returns false") {
        REQUIRE_FALSE(bmp.get_pixel(-1, 0));
        REQUIRE_FALSE(bmp.get_pixel(0, -1));
        REQUIRE_FALSE(bmp.get_pixel(32, 0));
        REQUIRE_FALSE(bmp.get_pixel(0, 16));
    }

    SECTION("out-of-bounds set is no-op") {
        bmp.set_pixel(-1, 0, true); // Should not crash
        bmp.set_pixel(32, 0, true); // Should not crash
        bmp.set_pixel(0, -1, true); // Should not crash
        bmp.set_pixel(0, 16, true); // Should not crash
    }
}

TEST_CASE("LabelBitmap::fill", "[label]") {
    auto bmp = LabelBitmap::create(16, 8);

    bmp.fill(true);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            REQUIRE(bmp.get_pixel(x, y));
        }
    }

    bmp.fill(false);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            REQUIRE_FALSE(bmp.get_pixel(x, y));
        }
    }
}

TEST_CASE("LabelBitmap::blit compositing", "[label]") {
    auto dst = LabelBitmap::create(20, 20);
    auto src = LabelBitmap::create(5, 5);
    src.fill(true);

    dst.blit(src, 3, 3);

    // Pixels inside blit region should be black
    REQUIRE(dst.get_pixel(3, 3));
    REQUIRE(dst.get_pixel(7, 7));

    // Pixels outside should be white
    REQUIRE_FALSE(dst.get_pixel(2, 3));
    REQUIRE_FALSE(dst.get_pixel(3, 2));
    REQUIRE_FALSE(dst.get_pixel(8, 3));
}

TEST_CASE("LabelBitmap::blit clipping", "[label]") {
    auto dst = LabelBitmap::create(10, 10);
    auto src = LabelBitmap::create(8, 8);
    src.fill(true);

    SECTION("clips at right/bottom edge") {
        dst.blit(src, 5, 5);
        // (5,5) to (9,9) should be black
        REQUIRE(dst.get_pixel(5, 5));
        REQUIRE(dst.get_pixel(9, 9));
        // (4,5) should still be white
        REQUIRE_FALSE(dst.get_pixel(4, 5));
    }

    SECTION("clips at left/top edge") {
        dst.blit(src, -3, -3);
        // (0,0) to (4,4) should be black
        REQUIRE(dst.get_pixel(0, 0));
        REQUIRE(dst.get_pixel(4, 4));
        // (5,0) should be white
        REQUIRE_FALSE(dst.get_pixel(5, 0));
    }
}

TEST_CASE("generate_qr_bitmap produces valid output", "[label]") {
    auto qr = helix::generate_qr_bitmap("web+spoolman:s-42", 200);

    REQUIRE_FALSE(qr.empty());
    REQUIRE(qr.width() > 0);
    REQUIRE(qr.height() > 0);
    // QR code is square
    REQUIRE(qr.width() == qr.height());
    // May be slightly larger than target due to module-size rounding (blit clips)
    REQUIRE(qr.width() <= 210);

    // Should have some black pixels (QR data)
    bool has_black = false;
    for (int y = 0; y < qr.height() && !has_black; y++) {
        for (int x = 0; x < qr.width() && !has_black; x++) {
            if (qr.get_pixel(x, y))
                has_black = true;
        }
    }
    REQUIRE(has_black);

    // Should also have some white pixels
    bool has_white = false;
    for (int y = 0; y < qr.height() && !has_white; y++) {
        for (int x = 0; x < qr.width() && !has_white; x++) {
            if (!qr.get_pixel(x, y))
                has_white = true;
        }
    }
    REQUIRE(has_white);
}

TEST_CASE("generate_qr_bitmap handles empty input", "[label]") {
    auto qr = helix::generate_qr_bitmap("", 200);
    REQUIRE(qr.empty());
}

TEST_CASE("generate_qr_bitmap handles small target", "[label]") {
    // Even with tiny target, should produce something
    auto qr = helix::generate_qr_bitmap("test", 10);
    REQUIRE_FALSE(qr.empty());
    REQUIRE(qr.width() >= 1);
}

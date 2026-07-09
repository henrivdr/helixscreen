// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;

TEST_CASE("Thumbnailer: cache key generation", "[timelapse][thumbnailer]") {
    SECTION("different videos produce different keys") {
        auto key1 = cache_key("benchy_20260312.mp4");
        auto key2 = cache_key("vase_spiral.mp4");
        REQUIRE(key1 != key2);
    }

    SECTION("same video produces same key") {
        auto key1 = cache_key("benchy.mp4");
        auto key2 = cache_key("benchy.mp4");
        REQUIRE(key1 == key2);
    }

    SECTION("key has timelapse prefix") {
        auto key = cache_key("test.mp4");
        REQUIRE(key.find("tl_") == 0);
    }
}

TEST_CASE("Thumbnailer: companion filename", "[timelapse][thumbnailer]") {
    SECTION("mp4 gets .thumb.jpg companion") {
        REQUIRE(companion_filename("benchy.mp4") == "benchy.thumb.jpg");
    }

    SECTION("mkv gets .thumb.jpg companion") {
        REQUIRE(companion_filename("print.mkv") == "print.thumb.jpg");
    }
}

TEST_CASE("Thumbnailer: ffmpeg argument list construction", "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/home/pi/printer_data/timelapse/benchy.mp4",
                                    "/home/pi/printer_data/timelapse/benchy.thumb.jpg");

    REQUIRE(args.size() == 9);
    REQUIRE(args[0] == "ffmpeg");
    REQUIRE(args[1] == "-y");
    REQUIRE(args[2] == "-i");
    REQUIRE(args[3] == "/home/pi/printer_data/timelapse/benchy.mp4");
    REQUIRE(args[4] == "-vframes");
    REQUIRE(args[5] == "1");
    REQUIRE(args[6] == "-q:v");
    REQUIRE(args[7] == "3");
    REQUIRE(args[8] == "/home/pi/printer_data/timelapse/benchy.thumb.jpg");
}

TEST_CASE("Thumbnailer: ffmpeg args are safe from shell injection", "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/tmp/$(rm -rf /).mp4", "/tmp/out.jpg");

    // The malicious filename is passed as a single argument element, not shell-interpreted
    REQUIRE(args[3] == "/tmp/$(rm -rf /).mp4");
}

TEST_CASE("Thumbnailer: video file filtering", "[timelapse][thumbnailer]") {
    REQUIRE(is_video_file("benchy.mp4") == true);
    REQUIRE(is_video_file("print.mkv") == true);
    REQUIRE(is_video_file("timelapse.avi") == true);
    REQUIRE(is_video_file("thumbnail.jpg") == false);
    REQUIRE(is_video_file("benchy.thumb.jpg") == false);
    REQUIRE(is_video_file("config.cfg") == false);
    REQUIRE(is_video_file("") == false);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "ams_types.h"
#include "consumption_sink.h"

#include "../catch_amalgamated.hpp"

using helix::ExternalSpoolSink;

namespace {

// Uses LVGLTestFixture because AmsState::init_subjects + lv_tick_get require
// LVGL initialization. Mirrors the existing tracker tests.
struct ExternalSpoolSinkFixture : LVGLTestFixture {
    ExternalSpoolSinkFixture() {
        auto& ams = AmsState::instance();
        ams.init_subjects(false);

        SlotInfo info;
        info.material = "PLA";
        info.remaining_weight_g = 1000.0f;
        info.total_weight_g = 1000.0f;
        ams.set_external_spool_info_in_memory(info);
    }

    ~ExternalSpoolSinkFixture() override {
        AmsState::instance().clear_external_spool_info();
    }
};

} // namespace

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: snapshot captures baseline when trackable",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: apply_delta decrements remaining_weight_g",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    // 1000 mm of 1.75mm PLA @ 1.24 g/cm^3 ≈ 2.98 g.
    sink.apply_delta(1000.0f);
    auto info = AmsState::instance().get_external_spool_info();
    REQUIRE(info.has_value());
    REQUIRE(info->remaining_weight_g < 1000.0f);
    REQUIRE(info->remaining_weight_g > 996.0f);
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: unknown weight not trackable",
                 "[consumption_sink][external]") {
    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = -1.0f;
    AmsState::instance().set_external_spool_info_in_memory(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: unknown material not trackable",
                 "[consumption_sink][external]") {
    SlotInfo info;
    info.material = "UnknownNovelMaterial9000";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    AmsState::instance().set_external_spool_info_in_memory(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: no external spool info not trackable",
                 "[consumption_sink][external]") {
    AmsState::instance().clear_external_spool_info();

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: apply_delta clamps remaining weight at zero",
                 "[consumption_sink][external]") {
    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 5.0f; // only 5 g available
    info.total_weight_g = 1000.0f;
    AmsState::instance().set_external_spool_info_in_memory(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    // Consume ~29.8 g (would drive negative without clamp).
    sink.apply_delta(10000.0f);
    auto after = AmsState::instance().get_external_spool_info();
    REQUIRE(after.has_value());
    REQUIRE(after->remaining_weight_g == 0.0f);
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: external write rebaselines instead of applying delta",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    sink.apply_delta(1000.0f); // ~3 g consumed → ~997 g remaining

    // Simulate an external writer replacing remaining_weight_g.
    auto current = AmsState::instance().get_external_spool_info();
    REQUIRE(current.has_value());
    SlotInfo edited = *current;
    edited.remaining_weight_g = 500.0f;
    AmsState::instance().set_external_spool_info_in_memory(edited);

    // Next tick detects the external write and rebaselines (no decrement).
    sink.apply_delta(1100.0f);
    auto snap_after_rebase = AmsState::instance().get_external_spool_info();
    REQUIRE(snap_after_rebase->remaining_weight_g == 500.0f);

    // Subsequent extrusion decrements from 500g, not from the original baseline.
    sink.apply_delta(1200.0f); // only 100 mm past rebase
    auto after = AmsState::instance().get_external_spool_info();
    REQUIRE(after->remaining_weight_g < 500.0f);
    REQUIRE(after->remaining_weight_g > 499.0f);
}

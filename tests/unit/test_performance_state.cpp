// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"
#include "performance_state.h"
#include "ui_update_queue.h"

#include <lvgl.h>

using helix::perf::PerformanceState;

namespace {

class PerfStateFixture : public HelixTestFixture {
  public:
    PerfStateFixture() {
        PerformanceState::instance().init_subjects();
    }
    ~PerfStateFixture() override {
        PerformanceState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState registers static subjects",
                 "[performance]") {
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_c10") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_free_mb") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_pct_used") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_state") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_text") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_names") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_about_summary") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_available") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_history_tick") != nullptr);

    // Defaults: nothing present, summary em-dash, available=0
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_available")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 0);
}

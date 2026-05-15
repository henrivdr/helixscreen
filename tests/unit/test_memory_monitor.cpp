// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "memory_monitor.h"

#include <atomic>

#include "../catch_amalgamated.hpp"

using namespace helix;

// =============================================================================
// MemoryThresholds::for_device()
// =============================================================================

TEST_CASE("MemoryThresholds for constrained device", "[memory_monitor]") {
    MemoryInfo info;
    info.total_kb = 110 * 1024; // 110MB — AD5M
    info.available_kb = 60 * 1024;

    auto t = MemoryThresholds::for_device(info);

    REQUIRE(t.warn_rss_kb == 30 * 1024);
    REQUIRE(t.critical_rss_kb == 40 * 1024);
    REQUIRE(t.warn_available_kb == 15 * 1024);
    REQUIRE(t.critical_available_kb == 8 * 1024);
    REQUIRE(t.growth_5min_kb == 1 * 1024);

    // Hysteresis: clear at 90% for RSS, 110% for available
    REQUIRE(t.clear_warn_rss_kb == t.warn_rss_kb * 90 / 100);
    REQUIRE(t.clear_critical_rss_kb == t.critical_rss_kb * 90 / 100);
    REQUIRE(t.clear_warn_available_kb == t.warn_available_kb * 110 / 100);
    REQUIRE(t.clear_critical_available_kb == t.critical_available_kb * 110 / 100);
}

TEST_CASE("MemoryThresholds for normal device", "[memory_monitor]") {
    MemoryInfo info;
    info.total_kb = 400 * 1024; // 400MB
    info.available_kb = 200 * 1024;

    auto t = MemoryThresholds::for_device(info);

    REQUIRE(t.warn_rss_kb == 120 * 1024);
    REQUIRE(t.critical_rss_kb == 180 * 1024);
}

TEST_CASE("MemoryThresholds for good device", "[memory_monitor]") {
    MemoryInfo info;
    info.total_kb = 2048 * 1024; // 2GB
    info.available_kb = 1500 * 1024;

    auto t = MemoryThresholds::for_device(info);

    // "good" tier scales RSS as a fraction of total RAM, with 180MB/230MB floors:
    //   warn     = max(180MB, total * 30%)
    //   critical = max(230MB, total * 50%)
    // At 2GB total, the scaled values win.
    REQUIRE(t.warn_rss_kb == info.total_kb * 30 / 100);
    REQUIRE(t.critical_rss_kb == info.total_kb * 50 / 100);
}

TEST_CASE("MemoryThresholds for good device honors RSS floor on small good devices",
          "[memory_monitor]") {
    // 512MB device: 30% = 153MB < 180MB floor; 50% = 256MB > 230MB floor.
    MemoryInfo info;
    info.total_kb = 512 * 1024;
    info.available_kb = 400 * 1024;

    auto t = MemoryThresholds::for_device(info);

    REQUIRE(t.warn_rss_kb == 180u * 1024);
    REQUIRE(t.critical_rss_kb == info.total_kb * 50 / 100);
}

// =============================================================================
// compute_pressure_level() — pure function tests
// =============================================================================

TEST_CASE("compute_pressure_level: no pressure when under thresholds", "[memory_monitor]") {
    MemoryStats stats;
    stats.vm_rss_kb = 10 * 1024; // 10MB — well under any threshold

    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;

    auto t = MemoryThresholds::for_device(sys);

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::none);
}

TEST_CASE("compute_pressure_level: warning when RSS exceeds warn threshold", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    MemoryStats stats;
    stats.vm_rss_kb = t.warn_rss_kb + 1024; // Above warn

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::warning);
}

TEST_CASE("compute_pressure_level: critical when RSS exceeds critical threshold",
          "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    MemoryStats stats;
    stats.vm_rss_kb = t.critical_rss_kb + 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::critical);
}

TEST_CASE("compute_pressure_level: elevated from growth", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    MemoryStats stats;
    stats.vm_rss_kb = 50 * 1024; // Under RSS thresholds

    int64_t growth = static_cast<int64_t>(t.growth_5min_kb) + 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, growth);
    REQUIRE(level == MemoryPressureLevel::elevated);
}

TEST_CASE("compute_pressure_level: available memory triggers warning", "[memory_monitor]") {
    MemoryInfo sys_init;
    sys_init.total_kb = 2048 * 1024;
    sys_init.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys_init);

    // Set available between critical and warn thresholds
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = (t.warn_available_kb + t.critical_available_kb) / 2;

    MemoryStats stats;
    stats.vm_rss_kb = 50 * 1024; // Under RSS threshold

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::warning);
}

TEST_CASE("compute_pressure_level: available memory triggers critical", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 10 * 1024; // Below critical_available_kb (24MB for good tier)
    auto t = MemoryThresholds::for_device(sys);

    MemoryStats stats;
    stats.vm_rss_kb = 50 * 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::critical);
}

// =============================================================================
// Hysteresis tests
// =============================================================================

TEST_CASE("compute_pressure_level: hysteresis holds warning level", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS is between clear and warn thresholds — should hold warning
    MemoryStats stats;
    stats.vm_rss_kb = (t.clear_warn_rss_kb + t.warn_rss_kb) / 2;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::warning, sys, 0);
    REQUIRE(level == MemoryPressureLevel::warning);
}

TEST_CASE("compute_pressure_level: hysteresis clears warning when below clear threshold",
          "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS below clear threshold — should drop to none
    MemoryStats stats;
    stats.vm_rss_kb = t.clear_warn_rss_kb - 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::warning, sys, 0);
    REQUIRE(level == MemoryPressureLevel::none);
}

TEST_CASE("compute_pressure_level: hysteresis holds critical level", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS between clear_critical and critical — should hold critical
    MemoryStats stats;
    stats.vm_rss_kb = (t.clear_critical_rss_kb + t.critical_rss_kb) / 2;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::critical, sys, 0);
    REQUIRE(level == MemoryPressureLevel::critical);
}

TEST_CASE("compute_pressure_level: hysteresis clears critical when below clear threshold",
          "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS below clear_critical but above clear_warn — should drop to warning hold
    // Wait: if RSS is below clear_critical_rss but above clear_warn_rss,
    // and current is critical, what happens?
    // The function checks: critical trigger? no. warn trigger? depends on value.
    // Then checks: hold critical? RSS >= clear_critical? no (we set it below).
    // Then checks: hold warning? RSS >= clear_warn? yes.
    // So it drops from critical to warning (held by hysteresis).
    MemoryStats stats;
    stats.vm_rss_kb = t.clear_critical_rss_kb - 1024;
    // Ensure this is still above clear_warn
    REQUIRE(stats.vm_rss_kb > t.clear_warn_rss_kb);

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::critical, sys, 0);
    // Critical cleared, but warning hysteresis still holds (RSS > clear_warn)
    REQUIRE(level == MemoryPressureLevel::warning);
}

TEST_CASE("compute_pressure_level: available memory hysteresis", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    MemoryStats stats;
    stats.vm_rss_kb = 50 * 1024; // Under RSS thresholds

    SECTION("holds warning when available between trigger and clear") {
        // Available above trigger (so wouldn't freshly trigger) but below clear
        sys.available_kb = (t.warn_available_kb + t.clear_warn_available_kb) / 2;

        auto level = compute_pressure_level(stats, t, MemoryPressureLevel::warning, sys, 0);
        REQUIRE(level == MemoryPressureLevel::warning);
    }

    SECTION("clears warning when available rises above clear threshold") {
        sys.available_kb = t.clear_warn_available_kb + 1024;

        auto level = compute_pressure_level(stats, t, MemoryPressureLevel::warning, sys, 0);
        REQUIRE(level == MemoryPressureLevel::none);
    }
}

// =============================================================================
// Zero-stats guard (non-Linux)
// =============================================================================

TEST_CASE("compute_pressure_level: zero stats produce no pressure", "[memory_monitor]") {
    MemoryStats stats; // All zeros
    MemoryInfo sys;
    sys.total_kb = 0;
    sys.available_kb = 0;
    auto t = MemoryThresholds::for_device(sys);

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::none);
}

// =============================================================================
// Pressure responder registry
// =============================================================================

TEST_CASE("MemoryMonitor pressure responder registration", "[memory_monitor]") {
    auto& monitor = MemoryMonitor::instance();

    SECTION("add and remove responder") {
        std::atomic<int> call_count{0};
        auto id =
            monitor.add_pressure_responder([&](MemoryPressureLevel) { call_count.fetch_add(1); });

        REQUIRE(id != 0);
        monitor.remove_pressure_responder(id);

        // Removing again is a no-op
        monitor.remove_pressure_responder(id);
    }

    SECTION("multiple responders get unique IDs") {
        auto id1 = monitor.add_pressure_responder([](MemoryPressureLevel) {});
        auto id2 = monitor.add_pressure_responder([](MemoryPressureLevel) {});

        REQUIRE(id1 != id2);

        monitor.remove_pressure_responder(id1);
        monitor.remove_pressure_responder(id2);
    }
}

// =============================================================================
// pressure_level_to_string
// =============================================================================

TEST_CASE("pressure_level_to_string", "[memory_monitor]") {
    REQUIRE(std::string(pressure_level_to_string(MemoryPressureLevel::none)) == "none");
    REQUIRE(std::string(pressure_level_to_string(MemoryPressureLevel::elevated)) == "elevated");
    REQUIRE(std::string(pressure_level_to_string(MemoryPressureLevel::warning)) == "warning");
    REQUIRE(std::string(pressure_level_to_string(MemoryPressureLevel::critical)) == "critical");
}

// =============================================================================
// Additional edge case tests
// =============================================================================

TEST_CASE("compute_pressure_level: critical drops directly to none", "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS drops below all clear thresholds in one step
    MemoryStats stats;
    stats.vm_rss_kb = t.clear_warn_rss_kb - 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::critical, sys, 0);
    REQUIRE(level == MemoryPressureLevel::none);
}

TEST_CASE("compute_pressure_level: RSS warning + available critical escalates to critical",
          "[memory_monitor]") {
    MemoryInfo sys;
    sys.total_kb = 2048 * 1024;
    sys.available_kb = 1500 * 1024;
    auto t = MemoryThresholds::for_device(sys);

    // RSS triggers warning, available triggers critical — should escalate to critical
    MemoryStats stats;
    stats.vm_rss_kb = t.warn_rss_kb + 1024;
    sys.available_kb = t.critical_available_kb - 1024;

    auto level = compute_pressure_level(stats, t, MemoryPressureLevel::none, sys, 0);
    REQUIRE(level == MemoryPressureLevel::critical);
}

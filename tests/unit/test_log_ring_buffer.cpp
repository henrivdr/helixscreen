// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the in-memory ring-buffer log sink and its bundle-collector
// integration. The ring buffer is the authoritative source for the debug
// bundle's log_tail: it always reflects the live process and always carries
// DEBUG-level detail, even when the persistent file/syslog sinks run at WARN
// (the AD5X failure mode — debug IFS logs never reach /var/log/messages so
// every bundle shipped stale, WARN-only context; #raza616).

#include "logging_init.h"
#include "system/log_collector.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::logging;

namespace {

// Reinitialize logging to a Console target so we never touch syslog/journal in
// the test environment, then return after the ring sink is installed. The
// process-global ring sink is rebuilt on every init(), so each test starts with
// a clean buffer.
void init_console_logging(spdlog::level::level_enum level = spdlog::level::warn) {
    LogConfig cfg;
    cfg.target = LogTarget::Console;
    cfg.enable_console = false; // keep test output quiet; ring sink is unaffected
    cfg.level = level;
    init(cfg);
}

} // namespace

// ============================================================================
// tail_ring_buffer() basics
// ============================================================================

TEST_CASE("tail_ring_buffer captures emitted lines newest-last", "[log_ring]") {
    init_console_logging(spdlog::level::info);

    spdlog::info("ring-marker-alpha");
    spdlog::info("ring-marker-beta");
    spdlog::info("ring-marker-gamma");

    auto tail = tail_ring_buffer(100);
    REQUIRE_FALSE(tail.empty());

    auto a = tail.find("ring-marker-alpha");
    auto b = tail.find("ring-marker-beta");
    auto g = tail.find("ring-marker-gamma");
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    REQUIRE(g != std::string::npos);

    // Newest-last ordering: alpha emitted first appears before gamma.
    REQUIRE(a < b);
    REQUIRE(b < g);
}

TEST_CASE("tail_ring_buffer captures DEBUG even when the logger level is WARN", "[log_ring]") {
    // The core fix: persistent sinks run at the user-configured level (WARN on
    // most devices) but the ring buffer must still see DEBUG so the bundle has
    // diagnostic detail. spdlog drops below the LOGGER level before any sink, so
    // init() raises the logger floor to debug and gates volume per-sink.
    init_console_logging(spdlog::level::warn);

    spdlog::debug("ring-debug-detail-12345");
    spdlog::warn("ring-warn-detail-67890");

    auto tail = tail_ring_buffer(100);
    REQUIRE(tail.find("ring-debug-detail-12345") != std::string::npos);
    REQUIRE(tail.find("ring-warn-detail-67890") != std::string::npos);
}

TEST_CASE("tail_ring_buffer bounds output to num_lines", "[log_ring]") {
    init_console_logging(spdlog::level::info);

    for (int i = 0; i < 50; ++i) {
        spdlog::info("ring-count-entry-{}", i);
    }

    auto tail = tail_ring_buffer(5);
    REQUIRE(tail.find("ring-count-entry-49") != std::string::npos);
    REQUIRE(tail.find("ring-count-entry-45") != std::string::npos);
    // Older entries beyond the requested window must not appear.
    REQUIRE(tail.find("ring-count-entry-44") == std::string::npos);
    REQUIRE(tail.find("ring-count-entry-0\n") == std::string::npos);
}

TEST_CASE("set_runtime_level keeps the ring buffer capturing DEBUG at WARN", "[log_ring]") {
    // The About-panel runtime level toggle must not starve the ring of debug.
    // A naive spdlog::set_level(warn) would set the logger floor to WARN and the
    // ring would miss debug; set_runtime_level() preserves the ring's floor.
    init_console_logging(spdlog::level::info);
    set_runtime_level(spdlog::level::warn);

    spdlog::debug("ring-runtime-debug-AABBCC");
    spdlog::warn("ring-runtime-warn-DDEEFF");

    auto tail = tail_ring_buffer(100);
    REQUIRE(tail.find("ring-runtime-debug-AABBCC") != std::string::npos);
    REQUIRE(tail.find("ring-runtime-warn-DDEEFF") != std::string::npos);

    // effective_log_level reflects the user-requested level, not the ring floor.
    REQUIRE(effective_log_level() == spdlog::level::warn);

    // Restore a sane level for subsequent tests sharing the process logger.
    set_runtime_level(spdlog::level::info);
}

// ============================================================================
// tail_best() prefers the ring buffer
// ============================================================================

TEST_CASE("tail_best prefers the ring buffer over file/syslog when populated", "[log_ring]") {
    init_console_logging(spdlog::level::info);

    spdlog::info("ring-tail-best-marker-XYZ");

    // No explicit paths → default cascade. The ring buffer is non-empty and the
    // current process, so it must win over any on-disk file the cascade finds.
    auto tail = helix::logs::tail_best(200);
    REQUIRE(tail.find("ring-tail-best-marker-XYZ") != std::string::npos);
}

TEST_CASE("tail_best honors an explicit paths list and skips the ring buffer", "[log_ring]") {
    // When the caller pins explicit paths (the file-resolution test seam), the
    // ring buffer must NOT shadow them — same contract as the effective-file
    // skip. Emit a ring marker, then ask for a path that doesn't exist: the
    // result must not contain the ring marker (cascade falls through to empty).
    init_console_logging(spdlog::level::info);
    spdlog::info("ring-should-not-leak-into-pinned-paths");

    auto tail = helix::logs::tail_best(50, {"/nonexistent/pinned.log"});
    REQUIRE(tail.find("ring-should-not-leak-into-pinned-paths") == std::string::npos);
}

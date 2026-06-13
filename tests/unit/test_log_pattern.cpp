// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure-string tests for the per-sink spdlog pattern decision. These guard the
// invariants that matter for debugging the async-delete crash family: every
// sink must carry the thread id (%t), console/file must carry a ms timestamp,
// the system sinks must NOT add their own time token (they would double-stamp
// the journal/syslog clock), and the console must keep its colored-level
// tokens. No real sinks are constructed — pattern_for_sink() is pure.

#include "logging_init.h"

#include "../catch_amalgamated.hpp"

#include <string>

using helix::logging::pattern_for_sink;
using helix::logging::SinkKind;

namespace {

bool contains(const char* hay, const char* needle) {
    return std::string(hay).find(needle) != std::string::npos;
}

// A pattern has a time token if it uses any of spdlog's clock fields.
bool has_time_token(const char* p) {
    return contains(p, "%H") || contains(p, "%e") || contains(p, "%T");
}

} // namespace

TEST_CASE("Every sink pattern includes the thread id", "[logging][pattern]") {
    // %t is the whole point of the change — losing it on any sink defeats the
    // main-thread-vs-background-thread diagnosis. This must FAIL if dropped.
    for (auto kind : {SinkKind::Console, SinkKind::File, SinkKind::Journald, SinkKind::Syslog,
                      SinkKind::Android, SinkKind::CrashBreadcrumb}) {
        INFO("kind index = " << static_cast<int>(kind));
        REQUIRE(contains(pattern_for_sink(kind), "%t"));
    }
}

TEST_CASE("Console and File patterns carry a ms timestamp", "[logging][pattern]") {
    // Nothing else stamps these streams, so they must include their own clock.
    REQUIRE(has_time_token(pattern_for_sink(SinkKind::Console)));
    REQUIRE(contains(pattern_for_sink(SinkKind::Console), "%e")); // ms precision
    REQUIRE(has_time_token(pattern_for_sink(SinkKind::File)));
    REQUIRE(contains(pattern_for_sink(SinkKind::File), "%e"));
}

TEST_CASE("System sinks do not add their own timestamp", "[logging][pattern]") {
    // journald/syslog/android stamp their own time; adding one would double up.
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Journald)));
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Syslog)));
    REQUIRE_FALSE(has_time_token(pattern_for_sink(SinkKind::Android)));
}

TEST_CASE("Console pattern preserves the colored level tokens", "[logging][pattern]") {
    const char* p = pattern_for_sink(SinkKind::Console);
    REQUIRE(contains(p, "%^"));
    REQUIRE(contains(p, "%$"));
}

TEST_CASE("System sinks keep level text for grep-ability", "[logging][pattern]") {
    // %l in syslog/journald is intentional (grep /var/log/messages by level).
    REQUIRE(contains(pattern_for_sink(SinkKind::Syslog), "%l"));
    REQUIRE(contains(pattern_for_sink(SinkKind::Journald), "%l"));
}

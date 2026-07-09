// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if IPerformanceSource gains a pure-virtual
// method and the concrete sources don't implement it, this fails to build.

#include "performance_source.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "mock_performance_source.h"
#include "moonraker_performance_source.h"

#include <type_traits>

TEST_CASE("MockPerformanceSource satisfies IPerformanceSource interface", "[compile][drift]") {
    using namespace helix::perf;
    static_assert(std::is_base_of_v<IPerformanceSource, MockPerformanceSource>,
                  "MockPerformanceSource must derive from IPerformanceSource");
    static_assert(
        !std::is_abstract_v<MockPerformanceSource>,
        "MockPerformanceSource must implement every pure virtual from IPerformanceSource");
    SUCCEED("IPerformanceSource ↔ MockPerformanceSource parity verified at compile time");
}

TEST_CASE("MoonrakerPerformanceSource satisfies IPerformanceSource interface", "[compile][drift]") {
    using namespace helix::perf;
    static_assert(std::is_base_of_v<IPerformanceSource, MoonrakerPerformanceSource>,
                  "MoonrakerPerformanceSource must derive from IPerformanceSource");
    static_assert(
        !std::is_abstract_v<MoonrakerPerformanceSource>,
        "MoonrakerPerformanceSource must implement every pure virtual from IPerformanceSource");
    SUCCEED("IPerformanceSource ↔ MoonrakerPerformanceSource parity verified at compile time");
}
#endif

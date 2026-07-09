// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if EthernetBackend adds a pure-virtual
// method and EthernetBackendMock doesn't implement it, this fails to build.

#include "ethernet_backend.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "ethernet_backend_mock.h"

TEST_CASE("EthernetBackendMock satisfies EthernetBackend interface", "[compile][drift]") {
    std::unique_ptr<EthernetBackend> p = std::make_unique<EthernetBackendMock>();
    REQUIRE(p != nullptr);
    // Exercise every pure-virtual so a non-abstract mock that happens to compile
    // but is missing overrides produces a link/runtime error rather than silent drift.
    (void)p->has_interface();
    EthernetInfo info = p->get_info();
    (void)info.connected;
}
#endif

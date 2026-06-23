// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if helix::IMoonrakerClient gains a pure-virtual
// method and neither helix::MoonrakerClient nor MoonrakerClientMock provides it,
// MoonrakerClientMock becomes abstract and this fails to build.

#include "i_moonraker_client.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_client_mock.h"

#include <type_traits>

TEST_CASE("MoonrakerClientMock satisfies helix::IMoonrakerClient interface", "[compile][drift]") {
    static_assert(std::is_base_of_v<helix::IMoonrakerClient, MoonrakerClientMock>,
                  "MoonrakerClientMock must derive from helix::IMoonrakerClient");
    static_assert(!std::is_abstract_v<MoonrakerClientMock>,
                  "MoonrakerClientMock must implement every pure virtual from IMoonrakerClient");
    SUCCEED("IMoonrakerClient ↔ MoonrakerClientMock parity verified at compile time");
}
#endif

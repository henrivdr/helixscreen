// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if IMoonrakerAPI gains a pure-virtual method
// and neither MoonrakerAPI (the real implementation) nor MoonrakerAPIMock
// provides it, MoonrakerAPIMock becomes abstract and this fails to build.

#include "i_moonraker_api.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_api_mock.h"

#include <type_traits>

TEST_CASE("MoonrakerAPIMock satisfies IMoonrakerAPI interface", "[compile][drift]") {
    static_assert(std::is_base_of_v<IMoonrakerAPI, MoonrakerAPIMock>,
                  "MoonrakerAPIMock must derive from IMoonrakerAPI");
    static_assert(!std::is_abstract_v<MoonrakerAPIMock>,
                  "MoonrakerAPIMock must implement every pure virtual from IMoonrakerAPI");
    SUCCEED("IMoonrakerAPI ↔ MoonrakerAPIMock parity verified at compile time");
}
#endif

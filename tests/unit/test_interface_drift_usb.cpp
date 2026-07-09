// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection for UsbBackend.

#include "usb_backend.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "usb_backend_mock.h"

TEST_CASE("UsbBackendMock satisfies UsbBackend interface", "[compile][drift]") {
    std::unique_ptr<UsbBackend> p = std::make_unique<UsbBackendMock>();
    REQUIRE(p != nullptr);
    (void)p->is_running();
    // start() and stop() exercised so their virtuals are linked; safe to call on mock.
    (void)p->start();
    p->stop();
}
#endif

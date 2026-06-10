// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_client.h"

namespace helix {

// Grants tests visibility into MoonrakerClient's install-once WebSocket callback
// state. Declared a friend of MoonrakerClient (see moonraker_client.h). Follows the
// existing TestAccess pattern (tests/test_helpers/, [L088]) rather than adding a
// production _for_testing() accessor.
class MoonrakerClientTestAccess {
  public:
    // True once install_ws_callbacks() has run for this client. The guard
    // ws_callbacks_installed_ is what ensures the inherited onopen/onmessage/onclose
    // std::functions are assigned exactly once and never reassigned per connect()
    // (the reassignment was the UAF in bundle UK9QCFY3).
    static bool callbacks_installed(const MoonrakerClient& c) {
        return c.ws_callbacks_installed_;
    }
};

} // namespace helix

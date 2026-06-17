// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "hv/json.hpp"

#include <string>

class MoonrakerAPI;

// Test-only accessor (declared in the test TU), forward-declared here so the
// friend grant below can name it explicitly.
struct GcodeNarrationRouterTestAccess;

namespace helix {
class MoonrakerClient;

/// Consumes `//` narration lines from notify_gcode_response and routes them to
/// the active AmsBackend's step model, updating the AmsState toolchange_step
/// subject. Sibling of GcodeErrorRouter; owns a SEPARATE subscription key.
/// Does NOT surface errors — it ignores `!!` / `Error:` / `ok` / status lines.
///
/// Lifetime: owned by `Application`. Registers a callback in the ctor and
/// unregisters it in the dtor; the MoonrakerClient pointer is not owned and
/// must outlive this router.
class GcodeNarrationRouter {
  public:
    GcodeNarrationRouter(MoonrakerAPI* api, MoonrakerClient* client);
    ~GcodeNarrationRouter();

    GcodeNarrationRouter(const GcodeNarrationRouter&) = delete;
    GcodeNarrationRouter& operator=(const GcodeNarrationRouter&) = delete;

  private:
    /// Test-only access to the private narration glue (`process_line`). The
    /// accessor lives in the global namespace (test TU), hence the leading `::`.
    friend struct ::GcodeNarrationRouterTestAccess;

    /// Live `notify_gcode_response` handler. Wrapped by `lifetime_.bg_cb` at
    /// the subscription layer, so the WHOLE body runs on the MAIN thread.
    void on_notify_gcode_response(const nlohmann::json& msg);

    /// Routes a single response line: only `//` narration is acted on. Runs on
    /// the main thread (bg_cb defers the notify body), so the synchronous
    /// AmsState::set_narration_phase write is thread-safe.
    void process_line(const std::string& line);

    MoonrakerAPI* api_;
    MoonrakerClient* client_;

    /// [L072] Generation guard for the callback captured by MoonrakerClient.
    /// Declared last so it destructs FIRST — outstanding tokens are invalidated
    /// before anything else the body might touch.
    AsyncLifetimeGuard lifetime_;
};

}  // namespace helix

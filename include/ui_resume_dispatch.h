// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

class MoonrakerAPI;

namespace helix::ui {

/// Run the active AMS backend's prepare_for_resume, then dispatch the
/// Resume StandardMacro. On either error path (prep failure or macro send
/// failure) emit a contextual `NOTIFY_ERROR` toast and invoke `on_failure`,
/// which call sites typically use to clear optimistic-UI state.
///
/// All work is async; this returns immediately. `on_failure` (when
/// provided) fires on the main thread.
///
/// @param api          Moonraker API to use for macro execution. Must
///                     outlive the dispatch — typically a panel member or
///                     a singleton-owned pointer. Must not be nullptr;
///                     callers handle the api == nullptr case (e.g. mock
///                     mode fallback) before calling.
/// @param log_prefix   Spdlog tag for log lines, e.g. `"[Print Status]"`.
///                     Stored by reference into the async lambdas — callers
///                     must pass a string-literal or otherwise long-lived
///                     `const char*`.
/// @param on_failure   Optional. Invoked on either error path AFTER the
///                     toast has been emitted. Default: no-op.
void dispatch_prepared_resume(MoonrakerAPI* api,
                              const char* log_prefix,
                              std::function<void()> on_failure = {});

} // namespace helix::ui

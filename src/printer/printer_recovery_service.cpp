// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_recovery_service.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

// Moonraker's error string when a [shell_command] is not defined. Anchored
// loosely — the wording has shifted across moonraker versions but the intent
// is consistent. We check both the structured error code (-32601 = method
// not found, JSON-RPC standard) and substring matches as a belt-and-braces.
bool looks_like_command_undefined(const MoonrakerError& err) {
    if (err.code == -32601) return true;
    const auto& m = err.message;
    return m.find("Method not found") != std::string::npos ||
           m.find("not found") != std::string::npos ||
           m.find("not registered") != std::string::npos ||
           m.find("unknown") != std::string::npos;
}

} // namespace

void PrinterRecoveryService::recover(SuccessCallback on_success, ErrorCallback on_error) {
    if (!api_) {
        spdlog::error("[Recovery] No Moonraker API — cannot recover");
        on_error(MoonrakerError::connection_lost("printer_recovery"));
        return;
    }

    // Capability gate: if Moonraker discovery told us shell_command isn't
    // loaded, skip the helix_recover RPC entirely. Saves ~3s of timeout on
    // hosts that can't host it (Bambu, RatOS-lite, vendor-locked Moonraker).
    if (!get_printer_state().is_shell_command_available()) {
        spdlog::info("[Recovery] shell_command component absent — going straight "
                     "to printer.firmware_restart");
        api_->restart_firmware(on_success, on_error);
        return;
    }

    spdlog::info("[Recovery] Trying [shell_command helix_recover]…");

    // [L072] Capture api_ by value, not bare `this` — the PrinterRecoveryService
    // may be a short-lived holder owned by a widget that gets destroyed between
    // click and ack. MoonrakerAPI lives as long as MoonrakerManager (well past
    // any widget). Caller's on_success/on_error captures are the caller's
    // problem to make lifetime-safe.
    MoonrakerAPI* api = api_;
    api_->run_shell_command(
        "helix_recover",
        [on_success](const std::string& output) {
            spdlog::info("[Recovery] helix_recover succeeded: {}",
                         output.empty() ? "(no output)" : output);
            on_success();
        },
        [api, on_success, on_error](const MoonrakerError& err) {
            if (looks_like_command_undefined(err)) {
                spdlog::info("[Recovery] helix_recover not defined on this platform "
                             "({}); falling back to printer.firmware_restart",
                             err.message);
                api->restart_firmware(on_success, on_error);
                return;
            }
            spdlog::warn("[Recovery] helix_recover failed: {}", err.message);
            on_error(err);
        });
}

} // namespace helix

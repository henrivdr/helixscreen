// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_narration_router.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <vector>

namespace helix {

namespace {

constexpr const char* kNotifyHandlerName = "gcode_narration_router";

}  // namespace

GcodeNarrationRouter::GcodeNarrationRouter(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    if (!client_) {
        spdlog::warn("[GcodeNarrationRouter] Null client — handler not registered");
        return;
    }

    // [L072] register_method_callback runs on the WS thread; MoonrakerClient
    // copies the callback list under lock and invokes outside it, so
    // unregister_method_callback in our dtor does NOT block in-flight
    // invocations. lifetime_.bg_cb wraps delivery: the WS thread queues the
    // body to the main thread with a generation snapshot; on main-thread
    // dispatch the gen is re-checked, so a callback that fires after the dtor
    // invalidates lifetime_ is silently dropped. Because the body is deferred
    // to main, on_notify_gcode_response (and process_line) run on the MAIN
    // thread — the direct AmsState::set_narration_phase write is therefore
    // thread-safe with no second defer.
    client_->register_method_callback(
        "notify_gcode_response", kNotifyHandlerName,
        lifetime_.bg_cb("GcodeNarrationRouter::on_notify",
                        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); }));
}

GcodeNarrationRouter::~GcodeNarrationRouter() {
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kNotifyHandlerName);
    }
}

void GcodeNarrationRouter::process_line(const std::string& line) {
    // Trim leading whitespace, then require the `//` narration prefix. Errors
    // (`!!` / `Error:`), `ok`, and status lines are ignored outright.
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) return;
    if (line.compare(start, 2, "//") != 0) return;

    // Strip the `//` prefix and any following whitespace.
    size_t body_start = line.find_first_not_of(" \t", start + 2);
    if (body_start == std::string::npos) return;
    std::string body = line.substr(body_start);

    // Runs on the main thread (the ctor's lifetime_.bg_cb wrapper defers the
    // notify body to main), so these synchronous AmsState accesses are safe.
    auto* backend = AmsState::instance().get_backend();
    if (!backend) return;

    auto id = backend->match_narration_phase(body);
    if (!id) return;

    const auto op = AmsState::instance().get_active_step_operation();
    const auto tmpl = backend->toolchange_phase_template(op);
    for (size_t k = 0; k < tmpl.size(); ++k) {
        if (tmpl[k].id == *id) {
            spdlog::debug("[GcodeNarration] phase '{}' -> step {} ({})", *id, k, tmpl[k].label);
            AmsState::instance().set_narration_phase(static_cast<int>(k), tmpl[k].label);
            return;
        }
    }
    // Matched a phase id the active operation's template doesn't contain — no
    // index to advance to; leave the step subject untouched.
}

void GcodeNarrationRouter::on_notify_gcode_response(const nlohmann::json& msg) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const auto& params = msg["params"];
    if (params[0].is_array()) {
        for (const auto& line : params[0]) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    } else if (params[0].is_string()) {
        for (const auto& line : params) {
            if (line.is_string()) {
                process_line(line.get<std::string>());
            }
        }
    }
}

}  // namespace helix

// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_error_router.h"

#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "ams_state.h"
#include "app_globals.h"
#include "error_classify.h"
#include "error_event.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "printer_recovery_service.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"
#include "rpc_error_correlation.h"

#if HELIX_HAS_CFS
#include "ams_backend_cfs.h"
#endif

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <vector>

namespace helix {

namespace {

constexpr const char* kNotifyHandlerName = "gcode_error_notifier";
constexpr const char* kReplayObserverName = "gcode_store_replay";

/// Maps RecoveryAction.style to PromptButton.color.
/// "primary" -> "primary", "danger" -> "error", anything else -> "" (neutral).
std::string color_for_style(const std::string& style) {
    if (style == "primary")
        return "primary";
    if (style == "danger")
        return "error";
    return ""; // neutral / theme default
}

/// Title for a plain CRITICAL modal (no recovery action). Preserves the prior
/// per-source behavior: CFS faults read "Filament System Error", anything else
/// the event's own title, falling back to "Printer Error". The classifier
/// leaves title empty, so the choice is derived from the source here.
/// NOTE: twin of modal_title_for() in recovery_modal_presenter.cpp (the
/// MODAL_WITH_RECOVER arm) — keep the CFS title rule in sync across both.
const char* modal_title_for(const ErrorEvent& e) {
    if (e.source == ErrorSource::CFS)
        return lv_tr("Filament System Error");
    return e.title.empty() ? lv_tr("Printer Error") : e.title.c_str();
}

/// Replay age gate: a latched `!!` older than this in the gcode_store is
/// considered stale and is NOT re-surfaced on reconnect.
///
/// Sized to cover the legitimate case -- an error fired during a brief
/// WebSocket bounce or boot-autostart hiccup that the user genuinely
/// missed -- while killing the stale-after-restart case (#991): a UI
/// restart on a paused print replayed a 287s-old `[print_task_config]`
/// error as a blocking modal that sat over the print panel and blocked
/// Resume. 30s comfortably spans a reconnect blip but is far below the
/// multi-minute gap a manual restart leaves. (Was 600s, which let the
/// 287s error through.)
constexpr double kReplayMaxAgeSeconds = 30.0;

/// gcode_store fetch depth on reconnect. The K2's box driver is chatty
/// (status polls every ~3s) so we need headroom to find a `!!` line.
constexpr int kReplayFetchCount = 50;

double now_unix_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

GcodeErrorRouter::GcodeErrorRouter(MoonrakerAPI* api, MoonrakerClient* client,
                                   helix::ui::RecoveryModalPresenter& presenter)
    : api_(api), client_(client), presenter_(presenter) {
    if (!client_) {
        spdlog::warn("[GcodeErrorRouter] Null client -- handlers not registered");
        return;
    }

    // [L072] Both registrations run on the WS thread. MoonrakerClient
    // copies the callback list under lock and invokes outside it, so
    // unregister_method_callback / remove_connected_observer in our dtor
    // do NOT block in-flight invocations. lifetime_.bg_cb wraps the
    // delivery: when the WS thread fires the wrapper, it queues `fn` to
    // the main thread with a generation snapshot; on main-thread dispatch
    // the gen is re-checked, so a callback that fires after the dtor
    // invalidates `lifetime_` is silently dropped.
    client_->register_method_callback(
        "notify_gcode_response", kNotifyHandlerName,
        lifetime_.bg_cb("GcodeErrorRouter::on_notify",
                        [this](const nlohmann::json& msg) { on_notify_gcode_response(msg); }));

    // Reconnect replay. Fires on WS open + Klippy ready transitions.
    // bg_cb takes a 0-arg callback fine -- the lambda below doesn't need
    // arguments; the wrapper just defers and gen-checks.
    client_->add_connected_observer(
        kReplayObserverName,
        lifetime_.bg_cb("GcodeErrorRouter::on_connected", [this]() { on_connected(); }));
}

GcodeErrorRouter::~GcodeErrorRouter() {
    // Erase the map entries so no NEW invocations start after this point.
    // In-flight invocations (already past the map lookup, queued for dispatch)
    // are handled by lifetime_'s generation guard -- see the bg_cb usage in
    // the ctor. lifetime_ destructs after this body returns and invalidates
    // all outstanding tokens, so any deferred body that lands on main after
    // the unregister is silently dropped.
    if (client_) {
        client_->unregister_method_callback("notify_gcode_response", kNotifyHandlerName);
        client_->remove_connected_observer(kReplayObserverName);
    }
}

bool GcodeErrorRouter::should_surface_replay(double entry_time, double now) {
    // Unavailable/zero timestamp: age cannot be positively determined.
    // Never suppress a possibly-fresh error on missing data -- preserve the
    // legacy surface-it behavior (the live `!!` path is unaffected by this
    // gate regardless).
    if (entry_time <= 0.0)
        return true;

    const double age = now - entry_time;
    // Clock skew or an entry stamped in the (apparent) future reads as a
    // negative age; treat as fresh rather than silently suppressing.
    if (age <= kReplayMaxAgeSeconds)
        return true;

    spdlog::debug("[GcodeError replay] Skipping stale `!!` (age {:.0f}s)", age);
    return false;
}

void GcodeErrorRouter::clean_error_text(std::string& text, std::string& out_code) {
    out_code.clear();

    // K2's Klipper builds emit errors in two shapes:
    //   1. Pure JSON:     `{"code":"key849","msg":"...","values":[...]}`
    //   2. Embedded JSON: `Internal error during connect: !{"code":"key298",...}`
    //      (observed K2 Plus 2026-05-24 when klipper_mcu shutdown)
    // Scan for the first `{"code":` anywhere in the line. If found, parse
    // from there; otherwise fall through to the heuristic rewrites.
    auto json_start = text.find("{\"code\"");
    if (json_start != std::string::npos) {
        // Brace-balance forward from json_start to find the matching close
        // brace, ignoring `{`/`}` inside string literals. nlohmann::parse
        // requires whole-input -- it won't ignore trailing garbage -- so we
        // extract just [json_start, obj_end) before parsing.
        size_t i = json_start;
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        size_t obj_end = std::string::npos;
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (in_string) {
                if (escape) {
                    escape = false;
                } else if (c == '\\') {
                    escape = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (--depth == 0) {
                    obj_end = i + 1;
                    break;
                }
            }
        }

        if (obj_end != std::string::npos) {
            std::string json_str = text.substr(json_start, obj_end - json_start);
            try {
                auto j = nlohmann::json::parse(json_str);
                if (j.contains("code") && j["code"].is_string()) {
                    out_code = j["code"].get<std::string>();
                    nlohmann::json values = nlohmann::json::array();
                    if (j.contains("values")) {
                        values = j["values"];
                    }
#if HELIX_HAS_CFS
                    if (auto friendly = printer::CfsErrorDecoder::lookup_message_with_values(
                            out_code, values)) {
                        text = friendly->first + ". " + friendly->second;
                        return;
                    }
#else
                    (void)values;
#endif
                }
                if (j.contains("msg") && j["msg"].is_string()) {
                    text = j["msg"].get<std::string>();
                }
            } catch (...) {
                // Malformed JSON despite the {"code" prefix -- leave text
                // untouched and fall through to heuristic patterns.
            }
        }
    }

    // Heuristic friendlier-text rewrites for common non-coded patterns.
    if (text.find("Must home axis") != std::string::npos ||
        text.find("must home") != std::string::npos) {
        text = lv_tr("Must home axes first");
        return;
    }
    if (text.find("spi_transfer_response") != std::string::npos) {
        text = lv_tr("Accelerometer communication failed. Try again.");
        return;
    }
}

std::string GcodeErrorRouter::truncate_for_toast(std::string text) {
    // UTF-8 byte truncation is not strictly correct (could land mid-codepoint),
    // but matches prior behavior. The right long-term fix is wrapping text in
    // ToastManager; at that point this goes away.
    constexpr size_t MAX_LEN = 80;
    if (text.size() > MAX_LEN) {
        text = text.substr(0, MAX_LEN - 3) + "...";
    }
    return text;
}

PresentAs decide_presentation(const ErrorEvent& e) {
    const bool has_recover = !e.recovery_actions.empty();
    if (e.severity == ErrorSeverity::CRITICAL)
        return has_recover ? PresentAs::MODAL_WITH_RECOVER : PresentAs::MODAL;
    if (e.severity == ErrorSeverity::WARNING)
        return has_recover ? PresentAs::TOAST_WITH_RECOVER : PresentAs::TOAST;
    return PresentAs::NONE; // INFO not surfaced in L0
}

PromptData build_recovery_prompt(const ErrorEvent& e) {
    PromptData p;
    p.title = e.title.empty() ? std::string(lv_tr("Printer Error")) : e.title;
    if (!e.detail.empty())
        p.text_lines.push_back(e.detail);
    for (const auto& a : e.recovery_actions) {
        PromptButton b;
        b.label = a.label;
        b.gcode = a.gcode;
        b.color = color_for_style(a.style);
        p.buttons.push_back(std::move(b));
    }
    return p;
}

void GcodeErrorRouter::present_recovery_modal(const ErrorEvent& e) {
    presenter_.present(e);
}

void GcodeErrorRouter::present_recover_toast(const ErrorEvent& e) {
    // key298 -- rpi MCU bridge daemon shutdown. firmware_restart alone
    // can't recover; PrinterRecoveryService bounces klipper_mcu via
    // the platform recovery script. The recovery action carries an EMPTY
    // gcode (log_tag error_classify::key298_recover) -- recovery runs
    // through PrinterRecoveryService, not execute_gcode.
    if (!api_)
        return; // No API client -> recovery would be a no-op; nothing actionable to show.
    MoonrakerAPI* api = api_;
    ToastManager::instance().show_with_action(
        ToastSeverity::ERROR, truncate_for_toast(e.detail).c_str(), lv_tr("Recover"),
        [](void* ud) {
            auto* a = static_cast<MoonrakerAPI*>(ud);
            if (!a)
                return;
            spdlog::info("[GcodeError] User tapped Recover for key298");
            PrinterRecoveryService recovery(a);
            recovery.recover(
                []() { spdlog::info("[Recovery] Auto-recovery initiated"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[Recovery] Auto-recovery failed: {}", err.message);
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        (std::string(lv_tr("Recovery failed: ")) + err.user_message()).c_str(),
                        6000);
                });
        },
        api, /*duration_ms=*/15000);
}

void GcodeErrorRouter::present_deferred_toast(const std::string& text) {
    // Deferred toast for unclassified errors -- gives the late-arrival
    // RPC error response a chance to populate the correlation buffer
    // before we re-check at fire time.
    struct DeferredCtx {
        std::string clean;
        std::string short_form;
    };
    auto* dctx = new DeferredCtx{text, truncate_for_toast(text)};
    auto* dt = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* c = static_cast<DeferredCtx*>(lv_timer_get_user_data(timer));
            if (c) {
                if (rpc_error_correlation::was_recently_handled(c->clean)) {
                    spdlog::info("[GcodeError] Suppressing deferred `!!` toast "
                                 "(caller-handled RPC error arrived after): {}",
                                 c->clean);
                } else {
                    ui_notification_error(lv_tr("Klipper Error"), c->short_form.c_str(),
                                          /*modal=*/false);
                }
                delete c;
            }
            lv_timer_delete(timer);
        },
        150, dctx);
    lv_timer_set_repeat_count(dt, 1);
}

void GcodeErrorRouter::process_line(const std::string& line) {
    if (line.empty())
        return;

    // Build classify context from current printer state. process_line runs
    // on the MAIN thread (the ctor's lifetime_.bg_cb wrapper defers the
    // notify body to main), so these synchronous getters are safe.
    ClassifyContext ctx;
    ctx.is_paused = get_printer_state().is_paused();
    ctx.is_printing = get_printer_state().get_print_job_state() == PrintJobState::PRINTING;

    // Ask the active AMS backend first (domain-aware); else the generic
    // classifier. get_backend() may return nullptr -- guarded.
    std::optional<ErrorEvent> ev;
    if (auto* backend = AmsState::instance().get_backend())
        ev = backend->classify_error(line, ctx);
    if (!ev)
        ev = error_classify::classify(line, ctx);
    if (!ev)
        return;

    spdlog::error("[GcodeError] sev={} src={} code={}: {}", static_cast<int>(ev->severity),
                  static_cast<int>(ev->source), ev->code.empty() ? "-" : ev->code, ev->detail);

    // Cross-source dedup: when an RPC caller triggered the gcode that
    // emitted this error, the caller's error_cb already surfaced a
    // contextual toast. Skipping our generic surfacing avoids double-
    // notification for the same root cause. (The deferred-toast path
    // re-checks at fire time for late-arriving RPC responses.)
    if (rpc_error_correlation::was_recently_handled(ev->detail)) {
        spdlog::info("[GcodeError] Suppressing duplicate (RPC-handled): {}", ev->detail);
        return;
    }

    switch (decide_presentation(*ev)) {
    case PresentAs::MODAL:
        // CRITICAL without a recovery action -- see modal_title_for().
        ui_notification_error(modal_title_for(*ev), ev->detail.c_str(), /*modal=*/true);
        return;
    case PresentAs::MODAL_WITH_RECOVER:
        present_recovery_modal(*ev);
        return;
    case PresentAs::TOAST_WITH_RECOVER:
        present_recover_toast(*ev);
        return;
    case PresentAs::TOAST:
        present_deferred_toast(ev->detail);
        return;
    case PresentAs::NONE:
        return;
    }
}

void GcodeErrorRouter::on_notify_gcode_response(const nlohmann::json& msg) {
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

void GcodeErrorRouter::on_connected() {
    if (!client_)
        return;
    // [L072] get_gcode_store's success callback fires on the WS thread when
    // Moonraker responds. The request tracker holds the callback for the
    // duration of the RPC, so a late response delivered after our dtor would
    // otherwise re-enter `this` on freed memory. bg_cb defers to main with
    // a generation guard.
    client_->get_gcode_store(
        kReplayFetchCount,
        lifetime_.bg_cb(
            "GcodeErrorRouter::replay_response",
            [this](const std::vector<GcodeStoreEntry>& entries) {
                // gcode_store is oldest-first; walk reverse for newest.
                for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                    if (it->type != "response")
                        continue;
                    const std::string& raw = it->message;
                    if (raw.size() < 3 || raw[0] != '!' || raw[1] != '!')
                        continue;

                    const double now = now_unix_seconds();
                    if (!should_surface_replay(it->time, now)) {
                        // should_surface_replay logs the stale skip at debug.
                        return;
                    }
                    const double age = now - it->time;

                    {
                        std::lock_guard<std::mutex> lock(replay_mutex_);
                        if (it->time == last_replayed_time_) {
                            spdlog::debug("[GcodeError replay] Already replayed t={}", it->time);
                            return;
                        }
                        last_replayed_time_ = it->time;
                    }

                    std::string clean =
                        (raw.size() > 3 && raw[2] == ' ') ? raw.substr(3) : raw.substr(2);
                    std::string code;
                    clean_error_text(clean, code);

                    spdlog::info(
                        "[GcodeError replay] Surfacing prior `!!` (age {:.0f}s, code={}): {}", age,
                        code.empty() ? "-" : code, clean);

                    // Replay is always modal -- the user was disconnected; a
                    // transient toast they can miss isn't enough on first
                    // reconnect. Modal dedup-by-title prevents the live
                    // notify_gcode_response (if Klippy re-emits) from
                    // duplicating.
                    const char* title = (code.size() >= 4 && code.compare(0, 4, "key8") == 0)
                                            ? lv_tr("Filament System Error")
                                            : lv_tr("Printer Error");
                    ui_notification_error(title, clean.c_str(), /*modal=*/true);
                    return;
                }
            }),
        [](const MoonrakerError& err) {
            spdlog::debug("[GcodeError replay] gcode_store query failed: {}", err.message);
        });
}

} // namespace helix

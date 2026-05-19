// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_resume_dispatch.h"

#include "ams_state.h"
#include "moonraker_api.h"
#include "standard_macros.h"
#include "ui_error_reporting.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

void dispatch_prepared_resume(MoonrakerAPI* api,
                              const char* log_prefix,
                              std::function<void()> on_failure) {
    if (!api) {
        spdlog::warn("{} dispatch_prepared_resume: api is null", log_prefix);
        if (on_failure) on_failure();
        return;
    }

    // Build the macro-dispatch closure. Both error and success leaf callbacks
    // capture `on_failure` by copy so the prep lambda can also carry its own
    // copy without competing for ownership.
    auto dispatch = [api, log_prefix, on_failure]() {
        StandardMacros::instance().execute(
            StandardMacroSlot::Resume, api,
            [log_prefix]() {
                spdlog::info("{} Resume command sent successfully", log_prefix);
            },
            [log_prefix, on_failure](const MoonrakerError& err) {
                spdlog::error("{} Failed to resume: {}", log_prefix, err.message);
                NOTIFY_ERROR(lv_tr("Failed to resume: {}"), err.user_message());
                if (on_failure) on_failure();
            },
            /*timeout_ms=*/0, /*suppress_auto_toast=*/true);
    };

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        dispatch();
        return;
    }

    int slot = backend->get_current_slot();
    backend->prepare_for_resume(
        slot, [dispatch, log_prefix, on_failure](const AmsError& err) {
            if (!err.success()) {
                spdlog::error("{} prepare_for_resume failed: {}", log_prefix, err.technical_msg);
                NOTIFY_ERROR(lv_tr("Resume preparation failed: {}"), err.user_msg);
                if (on_failure) on_failure();
                return;
            }
            dispatch();
        });
}

} // namespace helix::ui

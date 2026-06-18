// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recovery_modal_presenter.h"

#include "action_prompt_manager.h"
#include "error_event.h"
#include "moonraker_api.h"
#include "moonraker_error.h"
#include "moonraker_types.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_toast_manager.h"

#include "lvgl.h"

#include <spdlog/spdlog.h>

namespace {

/// Maps RecoveryAction.style to PromptButton.color.
/// "primary" -> "primary", "danger" -> "error", anything else -> "" (neutral).
std::string color_for_style(const std::string& style) {
    if (style == "primary") return "primary";
    if (style == "danger") return "error";
    return "";
}

/// Title for a CRITICAL recovery modal. Preserves the per-source behavior:
/// CFS faults read "Filament System Error", anything else the event title,
/// falling back to "Printer Error".
/// NOTE: twin of modal_title_for() in gcode_error_router.cpp (the plain
/// PresentAs::MODAL arm) — keep the CFS title rule in sync across both.
const char* modal_title_for(const helix::ErrorEvent& e) {
    if (e.source == helix::ErrorSource::CFS) return lv_tr("Filament System Error");
    return e.title.empty() ? lv_tr("Printer Error") : e.title.c_str();
}

}  // namespace

namespace helix::ui {

RecoveryModalPresenter::RecoveryModalPresenter(MoonrakerAPI* api) : api_(api) {}

bool RecoveryModalPresenter::is_visible() const {
    return modal_ && modal_->is_visible();
}

void RecoveryModalPresenter::dismiss() {
    if (modal_ && modal_->is_visible()) {
        modal_->hide();
    }
    shown_detail_.clear();
}

void RecoveryModalPresenter::present(const helix::ErrorEvent& e) {
    // Dedup: if the same detail is still on screen, do not re-show.
    // A dismissed-but-ongoing fault clears shown_detail_ so it can re-show.
    if (modal_ && modal_->is_visible() && e.detail == shown_detail_) {
        spdlog::debug("[RecoveryModalPresenter] Skipping duplicate (still visible): {}",
                      e.detail);
        return;
    }

    if (!modal_) {
        modal_ = std::make_unique<helix::ui::ActionPromptModal>();
        modal_->set_gcode_callback([this](const std::string& gcode) {
            // Runs on the main thread (button tap). Find the action for its log_tag.
            std::string tag = "RecoveryModalPresenter::recovery";
            for (const auto& a : active_actions_) {
                if (a.gcode == gcode) {
                    tag = a.log_tag;
                    break;
                }
            }
            shown_detail_.clear();  // user acted; allow re-show on a new fault
            spdlog::info("[RecoveryModal] User tapped recovery: {} ({})", tag, gcode);

            if (!api_) {
                spdlog::warn("[RecoveryModal] No API client; cannot execute gcode: {}", gcode);
                return;
            }
            api_->execute_gcode(
                gcode, [tag]() { spdlog::info("[Recovery] {} completed", tag); },
                [tag](const MoonrakerError& err) {
                    spdlog::error("[Recovery] {} failed: {}", tag, err.message);
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        ("Recovery failed: " + err.user_message()).c_str(), 6000);
                },
                MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
        });
    }

    active_actions_ = e.recovery_actions;
    shown_detail_ = e.detail;

    // Build the prompt. modal_title_for encodes the CFS "Filament System Error"
    // rule; build_recovery_prompt only knows e.title, which the classifier
    // leaves empty for CFS events.
    helix::PromptData prompt;
    prompt.title = modal_title_for(e);
    if (!e.detail.empty()) prompt.text_lines.push_back(e.detail);
    for (const auto& a : e.recovery_actions) {
        helix::PromptButton b;
        b.label = a.label;
        b.gcode = a.gcode;
        b.color = color_for_style(a.style);
        prompt.buttons.push_back(std::move(b));
    }
    // MODAL_WITH_RECOVER is always an error severity -- restore the red error affordance.
    prompt.severity = "error";

    lv_obj_t* screen = lv_screen_active();
    if (!screen || !modal_->show_prompt(screen, prompt)) {
        spdlog::warn("[RecoveryModal] show_prompt failed; falling back to alert");
        shown_detail_.clear();
        ui_notification_error(modal_title_for(e), e.detail.c_str(), /*modal=*/true);
    }
}

}  // namespace helix::ui

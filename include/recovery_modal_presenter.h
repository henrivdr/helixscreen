// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "action_prompt_manager.h" // PromptData / PromptButton
#include "action_prompt_modal.h"   // helix::ui::ActionPromptModal
#include "error_event.h"

#include <memory>
#include <string>
#include <vector>

class MoonrakerAPI;

namespace helix::ui {

/// Source-agnostic owner of the recovery modal shown for CRITICAL errors that
/// carry recovery actions (e.g. AFC jam, CFS key840). Lives as a member of
/// Application and is passed by reference to GcodeErrorRouter so the router
/// can delegate the modal presentation without owning any LVGL state.
///
/// Decoupling: the modal is created and shown regardless of whether api_ is
/// set. The api_ pointer is only used in the gcode-execution callback that
/// fires when the user taps a recovery button. This makes the presenter fully
/// testable with api_==nullptr.
///
/// Lifetime: must outlive any GcodeErrorRouter that holds a reference to it.
class RecoveryModalPresenter {
  public:
    explicit RecoveryModalPresenter(MoonrakerAPI* api);
    ~RecoveryModalPresenter() = default;

    RecoveryModalPresenter(const RecoveryModalPresenter&) = delete;
    RecoveryModalPresenter& operator=(const RecoveryModalPresenter&) = delete;

    /// Show the recovery modal for this event, or replace content if already
    /// visible. Deduplicates identical e.detail while the modal is on screen.
    /// Falls back to ui_notification_error when no screen is available.
    void present(const helix::ErrorEvent& e);

    /// Hide the modal if visible and clear shown-detail state so a subsequent
    /// present() with the same detail is not suppressed.
    void dismiss();

    [[nodiscard]] bool is_visible() const;

  private:
    MoonrakerAPI* api_;
    std::unique_ptr<helix::ui::ActionPromptModal> modal_;
    std::string shown_detail_;
    std::vector<helix::RecoveryAction> active_actions_;
};

} // namespace helix::ui

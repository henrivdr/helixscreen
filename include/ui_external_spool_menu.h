// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_context_menu.h"

#include "lvgl.h"

#include <functional>
#include <memory>

namespace helix::ui {

/// Show the External Spool context menu (Edit / Spoolman / Scan QR / Clear)
/// anchored to a canvas widget. Replaces the duplicated handle_bypass_click
/// body that used to live in both AmsPanel and AmsOverviewPanel.
///
/// @param parent_screen Parent screen for modal/overlay placement
/// @param anchor_widget Canvas (or other widget) the menu attaches to
/// @param context_menu  Owning panel's lazy-init unique_ptr — created on
///                      first use, reused afterwards
/// @param on_edit_action Fires for the EDIT and SPOOLMAN menu actions; the
///                      panel uses this to open its own edit modal. The bool
///                      argument requests the Spoolman spool picker directly
///                      (true for SPOOLMAN / "Select Spool", false for EDIT /
///                      "Spool Info"). The Scan QR and Clear actions go straight
///                      to AmsState and don't need a panel hook.
void show_external_spool_menu(lv_obj_t* parent_screen, lv_obj_t* anchor_widget,
                              std::unique_ptr<AmsContextMenu>& context_menu,
                              std::function<void(bool open_on_picker)> on_edit_action);

} // namespace helix::ui

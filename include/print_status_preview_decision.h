// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::ui {

/**
 * @brief What the print-status preview needs to (re)load to match desired state.
 *
 * Each flag is independent: the thumbnail (fallback content) and the gcode
 * viewer (3D/2D geometry) are reconciled separately.
 */
struct PreviewAction {
    bool load_thumbnail;
    bool load_gcode;
};

/**
 * @brief Decide what to (re)load so the preview matches the desired print.
 *
 * Pure function (no LVGL deps). The caller reads the *actual* widget state
 * (does the thumbnail have an image source? does the viewer hold geometry?)
 * and the desired state (the current print's effective filename, view mode,
 * lifecycle intent) and this function reconciles the two. Because the decision
 * is driven by real widget state rather than intent bools, re-entry is
 * self-healing: a blank widget always reloads.
 *
 * @param displayed_file    File whose content is CURRENTLY in the widgets
 *                          ("" = none/blank).
 * @param desired_file      The current print's effective filename
 *                          ("" = nothing to show).
 * @param thumbnail_has_src Does the thumbnail widget currently have an image
 *                          source.
 * @param gcode_has_content Does the gcode viewer currently hold geometry.
 * @param want_viewer       Lifecycle wants the 3D/2D viewer (vs thumbnail-only).
 * @param view_mode         0=thumbnail, 1=3D, 2=2D.
 * @return Which resources to (re)load.
 */
inline PreviewAction decide_preview_action(const std::string& displayed_file,
                                           const std::string& desired_file, bool thumbnail_has_src,
                                           bool gcode_has_content, bool want_viewer, int view_mode) {
    PreviewAction action{false, false};

    // Nothing to show: no print selected. Leave widgets untouched.
    if (desired_file.empty()) {
        return action;
    }

    const bool file_mismatch = (displayed_file != desired_file);

    // Thumbnail is always the fallback content beneath the viewer. (Re)load it
    // whenever the displayed file differs from desired or the widget is blank.
    if (file_mismatch || !thumbnail_has_src) {
        action.load_thumbnail = true;
    }

    // Gcode geometry is only relevant when the lifecycle wants the viewer and
    // the current view mode is 3D (1) or 2D (2). (Re)load when the file differs
    // or the viewer holds no geometry.
    if (want_viewer && (view_mode == 1 || view_mode == 2)) {
        if (file_mismatch || !gcode_has_content) {
            action.load_gcode = true;
        }
    }

    return action;
}

} // namespace helix::ui

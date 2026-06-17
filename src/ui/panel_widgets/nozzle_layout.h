// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout-decision logic for the Nozzle Temps dashboard widget.
//
// Deliberately free of LVGL / widget state so it can be unit-tested without a
// display, font subsystem, or UpdateQueue. The widget measures pixel widths
// (NozzleTempsWidget::on_size_changed) and feeds them here; this header decides
// how many columns to use and whether the long ("Nozzle 1") or short ("T0")
// label form fits.

namespace helix {

struct NozzleLayoutDecision {
    int columns = 1;            ///< 1 or 2 side-by-side columns of rows
    bool use_long_label = true; ///< true → "Nozzle 1", false → "T0"
};

/// Decide column count and label form from measured pixel widths.
///
/// @param avail_px    usable inner width of the tile content area (after padding)
/// @param gap_px      horizontal gap between two side-by-side rows
/// @param long_row_px px a single row needs at the long-label form
///                    (widest label + gap + widest value + comfort margin)
/// @param short_row_px px a single row needs at the short-label form
/// @param row_count   number of nozzle rows (columns clamped to this)
///
/// Pure arithmetic; no LVGL calls. A degenerate avail_px <= 0 (pre-layout)
/// returns the safe single-column long-label default.
[[nodiscard]] inline NozzleLayoutDecision
decide_nozzle_layout(int avail_px, int gap_px, int long_row_px, int short_row_px, int row_count) {
    if (avail_px <= 0)
        return {1, true};

    // Two columns only when there are at least two rows AND the short form of
    // both rows plus the inter-row gap fits the available width.
    int columns = (row_count >= 2 && avail_px >= 2 * short_row_px + gap_px) ? 2 : 1;

    // Never split a single row into two columns.
    if (columns > row_count)
        columns = row_count;
    if (columns < 1)
        columns = 1;

    int col_w = (columns == 2) ? (avail_px - gap_px) / 2 : avail_px;

    // Long label only when the per-column width comfortably fits the long form.
    bool use_long_label = (col_w >= long_row_px);

    return {columns, use_long_label};
}

} // namespace helix

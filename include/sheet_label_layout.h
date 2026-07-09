// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_bitmap.h"
#include "label_printer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix::label {

/// Standard paper sizes
enum class PaperSize {
    LETTER, // 8.5 x 11 inches (215.9 x 279.4 mm)
    A4,     // 210 x 297 mm
};

/// A sheet label template definition
struct SheetTemplate {
    std::string name;        // e.g., "Avery 5160"
    std::string description; // e.g., "30 labels, 1\" x 2-5/8\""
    PaperSize paper;

    // Label dimensions in mm (fractional for precision)
    float label_width_mm;
    float label_height_mm;

    // Grid layout
    int columns;
    int rows;

    // Margins from page edge to first label (mm)
    float margin_left_mm;
    float margin_top_mm;

    // Gaps between labels (mm)
    float gap_x_mm; // horizontal gap between columns
    float gap_y_mm; // vertical gap between rows

    // Computed helpers
    int labels_per_sheet() const {
        return columns * rows;
    }
};

/// Paper size dimensions at a given DPI
struct PageDimensions {
    int width_px;
    int height_px;
    float width_mm;
    float height_mm;
};

/// Get page dimensions for a paper size at given DPI
PageDimensions get_page_dimensions(PaperSize paper, int dpi);

/// Get the PWG media keyword for a paper size
/// (e.g., "na_letter_8.5x11in", "iso_a4_210x297mm")
const char* get_pwg_media_keyword(PaperSize paper);

/// Get all available sheet templates
const std::vector<SheetTemplate>& get_sheet_templates();

/// Get templates filtered by paper size
std::vector<const SheetTemplate*> get_templates_for_paper(PaperSize paper);

/// Convert a SheetTemplate to a LabelSize (for use with LabelRenderer).
/// The LabelSize will have dimensions matching a single label cell at the given DPI.
LabelSize sheet_template_to_label_size(const SheetTemplate& tmpl, int dpi = 300);

/// Tile a single label bitmap onto a full page.
/// Returns the full-page bitmap at the given DPI.
/// label_bitmap: the rendered label (will be centered if size doesn't match exactly)
/// tmpl: sheet template defining the grid layout
/// count: number of labels to place (1 to labels_per_sheet, fills left-to-right, top-to-bottom)
/// start: first slot to fill (0-based, skip already-used labels on a partial sheet)
/// dpi: output resolution (typically 300 or 600)
LabelBitmap tile_labels_on_page(const LabelBitmap& label_bitmap, const SheetTemplate& tmpl,
                                int count = 0, // 0 = fill entire sheet
                                int start = 0, // first slot index
                                int dpi = 300);

/// Get sheet template names as newline-separated string (for dropdown UI)
std::string sheet_template_options();

} // namespace helix::label

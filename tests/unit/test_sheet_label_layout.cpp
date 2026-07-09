// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_bitmap.h"
#include "sheet_label_layout.h"

#include "../catch_amalgamated.hpp"

using helix::LabelBitmap;
using namespace helix::label;

// ---------------------------------------------------------------------------
// Page dimensions
// ---------------------------------------------------------------------------

TEST_CASE("Page dimensions - Letter at 300 DPI", "[label][sheet]") {
    auto dims = get_page_dimensions(PaperSize::LETTER, 300);
    // 8.5" * 300 = 2550, 11" * 300 = 3300
    REQUIRE(dims.width_px == 2550);
    REQUIRE(dims.height_px == 3300);
    REQUIRE(dims.width_mm == Catch::Approx(215.9f).margin(0.1f));
    REQUIRE(dims.height_mm == Catch::Approx(279.4f).margin(0.1f));
}

TEST_CASE("Page dimensions - A4 at 300 DPI", "[label][sheet]") {
    auto dims = get_page_dimensions(PaperSize::A4, 300);
    // 210mm * 300 / 25.4 = 2480.3 -> 2480
    // 297mm * 300 / 25.4 = 3507.9 -> 3507
    REQUIRE(dims.width_px == Catch::Approx(2480).margin(2));
    REQUIRE(dims.height_px == Catch::Approx(3508).margin(2));
    REQUIRE(dims.width_mm == Catch::Approx(210.0f).margin(0.1f));
    REQUIRE(dims.height_mm == Catch::Approx(297.0f).margin(0.1f));
}

// ---------------------------------------------------------------------------
// PWG media keywords
// ---------------------------------------------------------------------------

TEST_CASE("PWG media keyword - Letter", "[label][sheet]") {
    REQUIRE(std::string(get_pwg_media_keyword(PaperSize::LETTER)) == "na_letter_8.5x11in");
}

TEST_CASE("PWG media keyword - A4", "[label][sheet]") {
    REQUIRE(std::string(get_pwg_media_keyword(PaperSize::A4)) == "iso_a4_210x297mm");
}

// ---------------------------------------------------------------------------
// Templates
// ---------------------------------------------------------------------------

TEST_CASE("Template count", "[label][sheet]") {
    const auto& templates = get_sheet_templates();
    REQUIRE(templates.size() == 10);
}

TEST_CASE("Template - Avery 5160", "[label][sheet]") {
    const auto& templates = get_sheet_templates();
    // Avery 5160 is the first template
    const auto& t = templates[0];
    REQUIRE(t.name == "Avery 5160");
    REQUIRE(t.columns == 3);
    REQUIRE(t.rows == 10);
    REQUIRE(t.labels_per_sheet() == 30);
    REQUIRE(t.paper == PaperSize::LETTER);
}

TEST_CASE("Template - Avery L7163", "[label][sheet]") {
    const auto& templates = get_sheet_templates();
    // Find L7163
    const SheetTemplate* found = nullptr;
    for (const auto& t : templates) {
        if (t.name == "Avery L7163") {
            found = &t;
            break;
        }
    }
    REQUIRE(found != nullptr);
    REQUIRE(found->columns == 2);
    REQUIRE(found->rows == 7);
    REQUIRE(found->labels_per_sheet() == 14);
    REQUIRE(found->paper == PaperSize::A4);
}

TEST_CASE("Templates for paper - Letter", "[label][sheet]") {
    auto letter_templates = get_templates_for_paper(PaperSize::LETTER);
    REQUIRE(letter_templates.size() == 6);
    for (const auto* t : letter_templates) {
        REQUIRE(t->paper == PaperSize::LETTER);
    }
}

TEST_CASE("Templates for paper - A4", "[label][sheet]") {
    auto a4_templates = get_templates_for_paper(PaperSize::A4);
    REQUIRE(a4_templates.size() == 4);
    for (const auto* t : a4_templates) {
        REQUIRE(t->paper == PaperSize::A4);
    }
}

// ---------------------------------------------------------------------------
// sheet_template_to_label_size
// ---------------------------------------------------------------------------

TEST_CASE("sheet_template_to_label_size", "[label][sheet]") {
    const auto& templates = get_sheet_templates();
    const auto& avery5160 = templates[0]; // 66.7mm x 25.4mm

    auto ls = sheet_template_to_label_size(avery5160, 300);

    // 66.7mm * 300 / 25.4 = 787.4 -> 787
    // 25.4mm * 300 / 25.4 = 300.0 -> 300
    REQUIRE(ls.width_px == Catch::Approx(787).margin(2));
    REQUIRE(ls.height_px == 300);
    REQUIRE(ls.dpi == 300);
    REQUIRE(ls.name == "Avery 5160");
}

// ---------------------------------------------------------------------------
// tile_labels_on_page
// ---------------------------------------------------------------------------

TEST_CASE("tile_labels_on_page - page dimensions", "[label][sheet]") {
    LabelBitmap label(10, 10);
    label.fill(true); // all black

    const auto& templates = get_sheet_templates();
    const auto& tmpl = templates[0]; // Avery 5160, Letter

    auto page = tile_labels_on_page(label, tmpl, 1, 0, 300);

    // Page should be Letter-sized at 300 DPI
    REQUIRE(page.width() == 2550);
    REQUIRE(page.height() == 3300);
}

TEST_CASE("tile_labels_on_page - fill count", "[label][sheet]") {
    LabelBitmap label(10, 10);
    label.fill(true);

    const auto& templates = get_sheet_templates();
    const auto& tmpl = templates[0]; // Avery 5160: 30 labels

    // count=0 fills all label slots
    auto page = tile_labels_on_page(label, tmpl, 0, 0, 300);

    // The small 10x10 label is centered within each cell.
    int label_w = static_cast<int>(tmpl.label_width_mm * 300 / 25.4f);
    int label_h = static_cast<int>(tmpl.label_height_mm * 300 / 25.4f);
    int offset_x = (label_w - 10) / 2;
    int offset_y = (label_h - 10) / 2;
    int margin_left = static_cast<int>(tmpl.margin_left_mm * 300 / 25.4f);
    int margin_top = static_cast<int>(tmpl.margin_top_mm * 300 / 25.4f);
    int gap_x = static_cast<int>(tmpl.gap_x_mm * 300 / 25.4f);
    int gap_y = static_cast<int>(tmpl.gap_y_mm * 300 / 25.4f);

    int placed = 0;
    for (int row = 0; row < tmpl.rows; ++row) {
        for (int col = 0; col < tmpl.columns; ++col) {
            int x = margin_left + col * (label_w + gap_x) + offset_x;
            int y = margin_top + row * (label_h + gap_y) + offset_y;
            if (page.get_pixel(x, y)) {
                placed++;
            }
        }
    }
    REQUIRE(placed == 30);
}

TEST_CASE("tile_labels_on_page - partial count", "[label][sheet]") {
    LabelBitmap label(10, 10);
    label.fill(true);

    const auto& templates = get_sheet_templates();
    const auto& tmpl = templates[0]; // Avery 5160

    auto page = tile_labels_on_page(label, tmpl, 5, 0, 300);

    int label_w = static_cast<int>(tmpl.label_width_mm * 300 / 25.4f);
    int label_h = static_cast<int>(tmpl.label_height_mm * 300 / 25.4f);
    int offset_x = (label_w - 10) / 2;
    int offset_y = (label_h - 10) / 2;
    int margin_left = static_cast<int>(tmpl.margin_left_mm * 300 / 25.4f);
    int margin_top = static_cast<int>(tmpl.margin_top_mm * 300 / 25.4f);
    int gap_x = static_cast<int>(tmpl.gap_x_mm * 300 / 25.4f);
    int gap_y = static_cast<int>(tmpl.gap_y_mm * 300 / 25.4f);

    // Count positions with black pixels (should be exactly 5)
    int placed = 0;
    for (int row = 0; row < tmpl.rows; ++row) {
        for (int col = 0; col < tmpl.columns; ++col) {
            int x = margin_left + col * (label_w + gap_x) + offset_x;
            int y = margin_top + row * (label_h + gap_y) + offset_y;
            if (page.get_pixel(x, y)) {
                placed++;
            }
        }
    }
    REQUIRE(placed == 5);
}

TEST_CASE("tile_labels_on_page - single label", "[label][sheet]") {
    LabelBitmap label(10, 10);
    label.fill(true);

    const auto& templates = get_sheet_templates();
    const auto& tmpl = templates[0]; // Avery 5160

    auto page = tile_labels_on_page(label, tmpl, 1, 0, 300);

    int label_w = static_cast<int>(tmpl.label_width_mm * 300 / 25.4f);
    int label_h = static_cast<int>(tmpl.label_height_mm * 300 / 25.4f);
    int offset_x = (label_w - 10) / 2;
    int offset_y = (label_h - 10) / 2;
    int margin_left = static_cast<int>(tmpl.margin_left_mm * 300 / 25.4f);
    int margin_top = static_cast<int>(tmpl.margin_top_mm * 300 / 25.4f);
    int gap_x = static_cast<int>(tmpl.gap_x_mm * 300 / 25.4f);

    // First label position (centered within cell) should have black pixels
    REQUIRE(page.get_pixel(margin_left + offset_x, margin_top + offset_y));

    // Second column center position should be white (only 1 label placed)
    int second_col_x = margin_left + (label_w + gap_x) + offset_x;
    REQUIRE_FALSE(page.get_pixel(second_col_x, margin_top + offset_y));
}

TEST_CASE("tile_labels_on_page - label placement", "[label][sheet]") {
    // Create a small label with a distinctive pattern
    LabelBitmap label(4, 4);
    label.fill(true); // all black

    const auto& templates = get_sheet_templates();
    const auto& tmpl = templates[0]; // Avery 5160

    auto page = tile_labels_on_page(label, tmpl, 2, 0, 300);

    int margin_left = static_cast<int>(tmpl.margin_left_mm * 300 / 25.4f);
    int margin_top = static_cast<int>(tmpl.margin_top_mm * 300 / 25.4f);
    int label_w = static_cast<int>(tmpl.label_width_mm * 300 / 25.4f);
    int gap_x = static_cast<int>(tmpl.gap_x_mm * 300 / 25.4f);

    // The small 4x4 label is centered within the cell.
    // Cell width for 5160 is ~787px, so offset_x = (787-4)/2 = 391
    int offset_x = (label_w - 4) / 2;
    int label_h = static_cast<int>(tmpl.label_height_mm * 300 / 25.4f);
    int offset_y = (label_h - 4) / 2;

    // First label: top-left cell, centered
    int x1 = margin_left + offset_x;
    int y1 = margin_top + offset_y;
    REQUIRE(page.get_pixel(x1, y1));
    REQUIRE(page.get_pixel(x1 + 3, y1 + 3)); // bottom-right of 4x4 label

    // Second label: second column, first row
    int x2 = margin_left + (label_w + gap_x) + offset_x;
    int y2 = margin_top + offset_y;
    REQUIRE(page.get_pixel(x2, y2));
}

// ---------------------------------------------------------------------------
// sheet_template_options
// ---------------------------------------------------------------------------

TEST_CASE("sheet_template_options", "[label][sheet]") {
    auto options = sheet_template_options();
    REQUIRE(!options.empty());
    // Should contain template names separated by newlines
    REQUIRE(options.find("Avery 5160") != std::string::npos);
    REQUIRE(options.find("Avery L7163") != std::string::npos);
    // Count newlines to verify correct count (10 templates = 9 newlines)
    int newlines = 0;
    for (char c : options) {
        if (c == '\n')
            newlines++;
    }
    REQUIRE(newlines == 9);
}

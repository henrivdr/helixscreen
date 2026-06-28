// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_markdown.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lv_markdown.h"
#include "lv_markdown_style.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

namespace {

/**
 * @brief Build a theme-aware markdown style from current design tokens
 *
 * Maps our theme system's responsive fonts, colors, and spacing to the
 * lv_markdown_style_t fields so the markdown renderer matches the app's
 * look and feel across screen sizes and light/dark themes.
 */
void build_theme_style(lv_markdown_style_t& style) {
    lv_markdown_style_init(&style);

    // Body text — responsive body font + theme text color
    style.body_font = theme_manager_get_font("font_body");
    style.body_color = theme_manager_get_color("text");

    // Headings — H1/H2 use heading font, H3/H4 use body, H5/H6 use small
    // H1/H2 get primary accent color, H3/H4 get bright text, H5/H6 get muted
    const lv_font_t* font_heading = theme_manager_get_font("font_heading");
    const lv_font_t* font_body = theme_manager_get_font("font_body");
    const lv_font_t* font_small = theme_manager_get_font("font_small");

    style.heading_font[0] = font_heading; // H1
    style.heading_font[1] = font_heading; // H2
    style.heading_font[2] = font_body;    // H3
    style.heading_font[3] = font_body;    // H4
    style.heading_font[4] = font_small;   // H5
    style.heading_font[5] = font_small;   // H6

    lv_color_t primary = theme_manager_get_color("primary");
    lv_color_t secondary = theme_manager_get_color("secondary");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t text_muted = theme_manager_get_color("text_muted");

    style.heading_color[0] = primary;    // H1 — primary accent
    style.heading_color[1] = secondary;  // H2 — secondary accent
    style.heading_color[2] = text_color; // H3 — bright
    style.heading_color[3] = text_color; // H4 — bright
    style.heading_color[4] = text_muted; // H5 — muted
    style.heading_color[5] = text_muted; // H6 — muted

    // Emphasis — NULL triggers faux bold (letter spacing) and underline fallbacks
    // We don't ship separate bold/italic font files, so rely on the fallbacks
    style.bold_font = nullptr;
    style.italic_font = nullptr;
    style.bold_italic_font = nullptr;

    // Inline code — small font, muted text on elevated surface
    style.code_font = font_small;
    style.code_color = theme_manager_get_color("text");
    style.code_bg_color = theme_manager_get_color("elevated_bg");
    style.code_corner_radius = theme_manager_get_spacing("space_xxs");

    // Fenced code blocks — same elevated surface, with padding
    style.code_block_bg_color = theme_manager_get_color("elevated_bg");
    style.code_block_corner_radius = theme_manager_get_spacing("space_xs");
    style.code_block_pad = theme_manager_get_spacing("space_sm");

    // Blockquotes — left border in muted color
    style.blockquote_border_color = theme_manager_get_color("primary");
    style.blockquote_border_width = 3;
    style.blockquote_pad_left = theme_manager_get_spacing("space_md");

    // Horizontal rules
    style.hr_color = theme_manager_get_color("text_muted");

    // Spacing — use themed spacing tokens for responsive values
    style.paragraph_spacing = theme_manager_get_spacing("space_sm");
    style.line_spacing = theme_manager_get_spacing("space_xxs");
    style.list_indent = theme_manager_get_spacing("space_lg");
}

/**
 * @brief Observer callback for bind_text subject changes
 *
 * Updates the markdown content when the bound string subject changes.
 */
void markdown_text_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* md_widget = lv_observer_get_target_obj(observer);
    if (!md_widget || !lv_obj_is_valid(md_widget)) {
        return;
    }

    const char* text = lv_subject_get_string(subject);
    // Skip empty/null inputs — md4c on tight-memory devices (e.g. K1, 209 MB RAM)
    // can null-deref deep inside md_analyze_inlines when an internal malloc fails.
    // Cap pathological lengths defensively too; release notes / telemetry text
    // is well under 64 KB in practice.
    if (!text || !*text) {
        lv_markdown_set_text(md_widget, "");
        return;
    }
    constexpr size_t kMarkdownMaxBytes = 256 * 1024;
    size_t len = strlen(text);
    if (len > kMarkdownMaxBytes) {
        spdlog::warn("[ui_markdown] Truncating oversized markdown ({} bytes > {} cap)", len,
                     kMarkdownMaxBytes);
        std::string truncated(text, kMarkdownMaxBytes);
        lv_markdown_set_text(md_widget, truncated.c_str());
        return;
    }
    lv_markdown_set_text(md_widget, text);
    spdlog::trace("[ui_markdown] Observer updated text ({} bytes)", len);
}

/**
 * @brief XML create callback for <ui_markdown> widget
 *
 * Creates a markdown viewer with theme-aware styling.
 * Note: lv_markdown_create() owns user_data for its internal lv_markdown_data_t.
 * lv_markdown_set_style() copies into that internal data, so no wrapper storage needed.
 */
void* ui_markdown_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create the underlying markdown widget (sets up its own user_data internally)
    lv_obj_t* obj = lv_markdown_create(parent);

    // Build theme-aware style and apply — style is copied into the widget's internal data
    lv_markdown_style_t style{};
    build_theme_style(style);
    lv_markdown_set_style(obj, &style);

    spdlog::trace("[ui_markdown] Created markdown widget");
    return obj;
}

/**
 * @brief XML apply callback for <ui_markdown> widget
 *
 * Handles standard object properties plus:
 * - bind_text: binds to a string subject for dynamic markdown content
 * - text: sets static markdown content
 */
void ui_markdown_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // Apply base object attributes (width, height, align, hidden, etc.)
    lv_xml_obj_apply(state, attrs);

    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);

    // Handle bind_text — bind to a string subject
    const char* bind_text = lv_xml_get_value_of(attrs, "bind_text");
    if (bind_text) {
        lv_subject_t* subject = lv_xml_get_subject(&state->scope, bind_text);
        if (subject) {
            lv_subject_add_observer_obj(subject, markdown_text_observer_cb, obj, nullptr);
            spdlog::trace("[ui_markdown] Bound to subject '{}'", bind_text);
        } else {
            spdlog::warn("[ui_markdown] Subject '{}' not found for bind_text", bind_text);
        }
    }

    // Handle static text attribute
    const char* text = lv_xml_get_value_of(attrs, "text");
    if (text) {
        lv_markdown_set_text(obj, text);
        spdlog::trace("[ui_markdown] Set static text ({} bytes)", strlen(text));
    }
}

} // namespace

void ui_markdown_init() {
    lv_xml_register_widget("ui_markdown", ui_markdown_create, ui_markdown_apply);
    spdlog::trace("[ui_markdown] Registered markdown widget");
}

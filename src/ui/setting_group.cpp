// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "setting_group.h"

#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

static uint32_t setting_group_visible_child_count(lv_obj_t* group) {
    uint32_t count = 0;
    uint32_t n = lv_obj_get_child_count(group);
    for (uint32_t i = 0; i < n; i++) {
        if (!lv_obj_has_flag(lv_obj_get_child(group, i), LV_OBJ_FLAG_HIDDEN))
            count++;
    }
    return count;
}

uint32_t setting_group_divider_count(lv_obj_t* group) {
    if (!group)
        return 0;
    uint32_t visible = setting_group_visible_child_count(group);
    return visible > 0 ? visible - 1 : 0;
}

// Pure render: draws a 1px border-colored line at the top edge of every visible
// child except the first visible one. Read-only over the child list — never
// mutates state (threading-safe inside the draw pipeline).
static void setting_group_draw_dividers(lv_event_t* e) {
    lv_obj_t* group = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!group || !layer)
        return;

    lv_area_t coords;
    lv_obj_get_coords(group, &coords);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = theme_manager_get_color("border");
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;

    uint32_t n = lv_obj_get_child_count(group);
    bool seen_visible = false;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(group, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            continue;
        if (!seen_visible) {
            seen_visible = true; // no line above the first visible child
            continue;
        }
        lv_area_t c;
        lv_obj_get_coords(child, &c);
        dsc.p1.x = coords.x1;
        dsc.p1.y = c.y1;
        dsc.p2.x = coords.x2;
        dsc.p2.y = c.y1;
        lv_draw_line(layer, &dsc);
    }
}

static void* setting_group_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);
    if (!obj) {
        spdlog::error("[SettingGroup] Failed to create lv_obj");
        return nullptr;
    }

    // Card shell: card_bg fill + theme border + theme border_radius (reactive).
    // Strip the LVGL theme's LV_PART_MAIN styles first so the shared style wins.
    lv_obj_remove_style(obj, nullptr, LV_PART_MAIN);
    lv_obj_add_style(obj, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);

    // Clip children to the rounded rect: first/last row corners round automatically,
    // honoring the theme radius fully (no clamp) including Pill/Full.
    lv_obj_set_style_clip_corner(obj, true, LV_PART_MAIN);

    // Full width; rows own their own internal padding, so the card has none.
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, 0, LV_PART_MAIN); // L055: flex gaps are separate from pad_all
    lv_obj_set_style_pad_column(obj, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);

    // Responsive inset: side margins float the card; bottom margin separates groups.
    // theme_manager_get_spacing() returns the value for the current breakpoint.
    int32_t side = theme_manager_get_spacing("space_sm");
    int32_t gap = theme_manager_get_spacing("space_md");
    lv_obj_set_style_margin_left(obj, side, LV_PART_MAIN);
    lv_obj_set_style_margin_right(obj, side, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(obj, gap, LV_PART_MAIN);

    // Fixed container, not a scroll area (the overlay content scrolls).
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    spdlog::trace("[SettingGroup] Created <setting_group> with card shell");

    // Auto-managed dividers between visible children (pure render).
    lv_obj_add_event_cb(obj, setting_group_draw_dividers, LV_EVENT_DRAW_POST, nullptr);

    return (void*)obj;
}

void setting_group_register(void) {
    lv_xml_register_widget("setting_group", setting_group_xml_create, lv_xml_obj_apply);
    spdlog::trace("[SettingGroup] Registered <setting_group> widget");
}

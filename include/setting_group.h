// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * Register the <setting_group> widget with the LVGL XML system.
 * A setting_group is a rounded, bordered card that lays its children out in a
 * zero-padding flex column and draws 1px dividers between visible children.
 * Card fill/border/radius come from the theme Card style (reactive to theme changes).
 * Must be called before any XML using <setting_group> is registered.
 */
void setting_group_register(void);

/**
 * Number of dividers that will be drawn for a group: one above every visible
 * direct child except the first visible one (0 when <=1 visible child).
 * Exposed for testing the visibility logic without pixel inspection.
 */
uint32_t setting_group_divider_count(lv_obj_t* group);

#ifdef __cplusplus
}
#endif

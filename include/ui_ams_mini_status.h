// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_mini_status.h
 * @brief Compact AMS status indicator widget for home panel
 *
 * The ams_mini_status widget shows a compact visualization of AMS filament slots
 * as vertical bar indicators. Each bar shows three pieces of information:
 *
 * 1. **Color**: Bar fill color = filament color
 * 2. **Presence**: Empty slots shown as gray/transparent
 * 3. **Fill level**: Bar fills from bottom based on remaining filament %
 *
 * Layout:
 * - Up to 8 vertical bars representing slots/lanes
 * - "+N" overflow indicator when more than max_visible slots exist
 * - Auto-hides when slot_count == 0
 *
 * Programmatic usage:
 * @code{.cpp}
 * lv_obj_t* indicator = ui_ams_mini_status_create(parent, 32);  // 32px height
 * ui_ams_mini_status_set_slot_count(indicator, 4);
 * ui_ams_mini_status_set_slot(indicator, 0, 0xFF5722, 75, true);  // Orange, 75% full, present
 * ui_ams_mini_status_set_slot(indicator, 1, 0x2196F3, 50, true);  // Blue, 50% full
 * ui_ams_mini_status_set_slot(indicator, 2, 0x000000, 0, false);  // Empty slot
 * @endcode
 */

/** Maximum number of visible slots (bars) in the compact view */
#define AMS_MINI_STATUS_MAX_VISIBLE 8

/**
 * @brief Create an AMS mini status indicator programmatically
 *
 * @param parent Parent LVGL object
 * @param height Height of the indicator in pixels (bars scale to this)
 * @return Created indicator object, or NULL on failure
 */
lv_obj_t* ui_ams_mini_status_create(lv_obj_t* parent, int32_t height);

/**
 * @brief Set the total number of slots
 *
 * If slot_count > max_visible, a "+N" overflow indicator is shown.
 * If slot_count == 0, the widget is hidden.
 *
 * @param obj The ams_mini_status widget
 * @param slot_count Total number of slots in the AMS system
 */
void ui_ams_mini_status_set_slot_count(lv_obj_t* obj, int slot_count);

/**
 * @brief Set the maximum number of visible slots
 *
 * @param obj The ams_mini_status widget
 * @param max_visible Maximum slots to show (1-8, default 8)
 */
void ui_ams_mini_status_set_max_visible(lv_obj_t* obj, int max_visible);

/**
 * @brief Update a single slot's display
 *
 * @param obj The ams_mini_status widget
 * @param slot_index Slot index (0 to max_visible-1)
 * @param color_rgb Filament color as 0xRRGGBB
 * @param fill_pct Fill percentage (0-100, 0=empty, 100=full)
 * @param present True if filament is present/available in this slot
 */
void ui_ams_mini_status_set_slot(lv_obj_t* obj, int slot_index, uint32_t color_rgb, int fill_pct,
                                 bool present);

/**
 * @brief Force refresh/redraw of all slots
 *
 * @param obj The ams_mini_status widget
 */
void ui_ams_mini_status_refresh(lv_obj_t* obj);

/**
 * @brief Set width + colspan hint for responsive sizing / mode selection.
 *
 * colspan >= 2 selects the wide spool view; colspan == 1 keeps the bar view.
 * @param obj The ams_mini_status widget
 * @param width_px Available pixel width for the widget
 * @param colspan Grid columns the widget spans (1 = narrow/bar, >=2 = wide/spool)
 */
void ui_ams_mini_status_set_width(lv_obj_t* obj, int width_px, int colspan);

/**
 * @brief Check if this is an ams_mini_status widget
 *
 * @param obj Object to check
 * @return true if this is an ams_mini_status widget
 */
bool ui_ams_mini_status_is_valid(lv_obj_t* obj);

/**
 * @brief Register ams_mini_status as an XML widget
 *
 * Call this once during application initialization to enable
 * using <ams_mini_status/> in XML layouts. The XML widget
 * automatically fills its parent and binds to AmsState.
 */
void ui_ams_mini_status_init(void);

#ifdef __cplusplus
}
#endif

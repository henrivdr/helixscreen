// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix::ui {

/** Reach state of a scroll container relative to its ends. */
struct PageScrollReach {
    bool can_up;       ///< true if the container can scroll further up
    bool can_down;     ///< true if the container can scroll further down
    bool has_overflow; ///< true if content overflows the viewport at all
};

/** One page-press scroll delta: ~90% of the viewport, so a row overlaps between
 *  pages. Floored at 1 px for any positive viewport; 0 for non-positive. */
inline int32_t page_scroll_step(int32_t viewport_height) {
    if (viewport_height <= 0) {
        return 0;
    }
    int32_t step = (viewport_height * 9) / 10; // integer 90%
    return step < 1 ? 1 : step;
}

/** Derive reach flags from LVGL scroll offsets.
 *  scroll_y      = lv_obj_get_scroll_y(container)       (>0 => room above)
 *  scroll_bottom = lv_obj_get_scroll_bottom(container)  (>0 => room below) */
inline PageScrollReach page_scroll_compute_reach(int32_t scroll_y, int32_t scroll_bottom) {
    PageScrollReach r;
    r.can_up = scroll_y > 0;
    r.can_down = scroll_bottom > 0;
    r.has_overflow = r.can_up || r.can_down;
    return r;
}

} // namespace helix::ui

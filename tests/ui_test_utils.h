// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

/**
 * @brief UI Test Utilities - Simulate user interactions and wait for UI updates
 *
 * Provides programmatic testing of LVGL UI components:
 * - Click/touch simulation
 * - Keyboard input simulation
 * - Async wait helpers (timers, animations, conditions)
 * - Widget state verification
 *
 * Usage:
 *   ui_test_init(screen);
 *   ui_test_click(button);
 *   ui_test_type_text(textarea, "password");
 *   ui_test_wait_ms(500);
 *   ui_test_cleanup();
 */

/**
 * @brief Safely initialize LVGL (idempotent - no warning if already initialized)
 *
 * Use this instead of calling lv_init() directly in tests to avoid
 * "lv_init: already initialized" warnings when tests run sequentially.
 */
void lv_init_safe();

namespace helix {
namespace ui {

/**
 * @brief Install a hook invoked by the test ui_notification_warning() stubs.
 *
 * Lets a test observe whether a user-facing WiFi warning toast was emitted
 * (the production path is stubbed to a log-only no-op in tests). Pass nullptr
 * to clear. The hook receives the formatted warning message.
 */
void set_test_notification_warning_hook(std::function<void(const std::string&)> hook);

} // namespace ui
} // namespace helix

namespace UITest {

/**
 * @brief Initialize UI test system with virtual input device
 * @param screen LVGL screen to attach input device to
 */
void init(lv_obj_t* screen);

/**
 * @brief Cleanup UI test system and remove virtual input device
 */
void cleanup();

/**
 * @brief Simulate click/touch on widget at its center
 * @param widget Widget to click
 * @return true if click was simulated successfully
 */
bool click(lv_obj_t* widget);

/**
 * @brief Simulate click/touch at specific coordinates
 * @param x X coordinate (absolute)
 * @param y Y coordinate (absolute)
 * @return true if click was simulated successfully
 */
bool click_at(int32_t x, int32_t y);

/**
 * @brief Type text into focused textarea character by character
 * @param text Text to type
 * @return true if text was sent successfully
 *
 * Note: Textarea must have focus before calling this function
 */
bool type_text(const std::string& text);

/**
 * @brief Type text into specific textarea (gives it focus first)
 * @param textarea Textarea widget to type into
 * @param text Text to type
 * @return true if text was sent successfully
 */
bool type_text(lv_obj_t* textarea, const std::string& text);

/**
 * @brief Send key press event (for special keys like Enter, Backspace)
 * @param key LV_KEY_* constant
 * @return true if key event was sent successfully
 */
bool send_key(uint32_t key);

/**
 * @brief Wait for specified milliseconds while processing LVGL tasks
 * @param ms Milliseconds to wait
 *
 * Processes lv_timer_handler() every 5ms during wait period
 */
void wait_ms(uint32_t ms);

/**
 * @brief Wait until condition becomes true or timeout expires
 * @param condition Function returning true when wait should end
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if condition became true, false if timeout
 *
 * Checks condition every 10ms, processing LVGL tasks between checks
 */
bool wait_until(std::function<bool()> condition, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for widget to become visible
 * @param widget Widget to wait for
 * @param timeout_ms Maximum time to wait
 * @return true if widget became visible, false if timeout
 */
bool wait_for_visible(lv_obj_t* widget, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for widget to become hidden
 * @param widget Widget to wait for
 * @param timeout_ms Maximum time to wait
 * @return true if widget became hidden, false if timeout
 */
bool wait_for_hidden(lv_obj_t* widget, uint32_t timeout_ms = 5000);

/**
 * @brief Wait for all pending timers to complete
 * @param timeout_ms Maximum time to wait
 * @return true if all timers completed, false if timeout
 *
 * Useful for waiting for async operations (scans, connections, etc.)
 */
bool wait_for_timers(uint32_t timeout_ms = 10000);

/**
 * @brief Check if widget is visible (not hidden)
 * @param widget Widget to check
 * @return true if widget is visible
 */
bool is_visible(lv_obj_t* widget);

/**
 * @brief Get text content from label or textarea
 * @param widget Label or textarea widget
 * @return Text content or empty string if not found
 */
std::string get_text(lv_obj_t* widget);

/**
 * @brief Check if widget is in checked/selected state
 * @param widget Checkbox, switch, or button widget
 * @return true if widget is checked/selected
 */
bool is_checked(lv_obj_t* widget);

/**
 * @brief Find widget by name within parent (recursive search)
 * @param parent Parent widget to search within
 * @param name Widget name to find
 * @return Widget if found, nullptr otherwise
 */
lv_obj_t* find_by_name(lv_obj_t* parent, const std::string& name);

/**
 * @brief Count children with specific user_data marker
 * @param parent Parent widget
 * @param marker User data marker string to match
 * @return Number of matching children
 *
 * Useful for counting dynamically created items (e.g., network list items)
 */
int count_children_with_marker(lv_obj_t* parent, const char* marker);

} // namespace UITest

/**
 * @brief Safe wrapper around lv_timer_handler() for tests
 *
 * Drains the UpdateQueue, then processes LVGL timers without triggering the
 * infinite do-while loop in lv_timer_handler().
 *
 * Strategy:
 *   1. Drain UpdateQueue (executes pending callbacks, subject observers fire)
 *   2. Pause ALL timers
 *   3. Manually fire ready one-shot timers (lv_async_call, retry timers)
 *   4. Re-pause ALL timers (one-shot callbacks may have unpaused timers,
 *      e.g. lv_obj_delete → lv_anim_delete → anim_mark_list_change resumes
 *      the animation timer)
 *   5. Call lv_timer_handler() with everything paused (updates internal state)
 *   6. Resume all timers
 *
 * Without step 4, unpaused timers with stale last_run timestamps enter
 * lv_timer_handler()'s do-while loop and never terminate because every fire
 * creates/deletes timers, restarting iteration from the head indefinitely.
 */
uint32_t lv_timer_handler_safe();

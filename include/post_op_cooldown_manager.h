// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <atomic>

/**
 * @brief Singleton manager for post-filament-operation heater cooldown
 *
 * After filament load/unload/swap operations, the extruder heater may be left on
 * indefinitely. This manager provides a unified cooldown path — all callers
 * (FilamentPanel, AMS sidebar, AMS backends) funnel through schedule()/cancel()
 * instead of implementing their own cooldown timers.
 *
 * - schedule() starts a configurable delay timer (default 120s)
 * - cancel() cancels a pending cooldown (user manually heats or new op starts)
 * - At fire time, checks: extruder target > 0 AND print state != PRINTING/PAUSED
 * - Calling schedule() resets any existing pending timer
 *
 * Thread safety: schedule() and cancel() are safe to call from any thread —
 * they defer to the LVGL main thread via queue_update().
 */
class PostOpCooldownManager {
  public:
    static PostOpCooldownManager& instance();

    /// Initialize the manager. Call once during startup.
    void init();

    /// Schedule a cooldown after the configured delay.
    /// Resets any existing pending timer. Safe to call from any thread.
    void schedule();

    /// Cancel any pending cooldown. Call when user manually heats
    /// or a new filament operation starts.
    void cancel();

    /// Shutdown — cancel timer, release resources. Call during app teardown.
    void shutdown();

  private:
    PostOpCooldownManager() = default;
    ~PostOpCooldownManager() = default;
    PostOpCooldownManager(const PostOpCooldownManager&) = delete;
    PostOpCooldownManager& operator=(const PostOpCooldownManager&) = delete;

    lv_timer_t* timer_ = nullptr;
    std::atomic<bool> initialized_{false};
};

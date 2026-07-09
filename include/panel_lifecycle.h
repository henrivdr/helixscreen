// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file panel_lifecycle.h
 * @brief Common lifecycle interface for panels and overlays
 *
 * Defines the minimal interface that both PanelBase and OverlayBase
 * implement for NavigationManager to dispatch lifecycle events.
 *
 * ## Implemented by:
 * - PanelBase: Main UI panels (enum-indexed, setup() pattern)
 * - OverlayBase: Modal overlays (widget-indexed, create() pattern)
 *
 * ## Lifecycle Contract:
 * - on_deactivate() called BEFORE a panel/overlay becomes hidden
 * - on_activate() called AFTER animation completes and panel/overlay is visible
 * - get_name() used for debugging/logging only
 *
 * @threading Main thread only
 */

#pragma once

/**
 * @class IPanelLifecycle
 * @brief Common lifecycle interface for NavigationManager dispatch
 *
 * This interface enables NavigationManager to handle both panels and overlays
 * polymorphically for lifecycle event dispatch.
 */
class IPanelLifecycle {
  public:
    virtual ~IPanelLifecycle() = default;

    /**
     * @brief Called when panel/overlay becomes visible
     *
     * Used to start background operations (scanning, subscriptions, timers).
     * Safe to call multiple times (implementations should be idempotent).
     */
    virtual void on_activate() = 0;

    /**
     * @brief Called when panel/overlay is being hidden
     *
     * Used to stop background operations before animation starts.
     * Safe to call multiple times (implementations should be idempotent).
     */
    virtual void on_deactivate() = 0;

    /**
     * @brief Tear down and re-create this view from its XML component definition
     *
     * Dev-only hook called by NavigationManager after XML hot-reload re-registers
     * a component. Default is a no-op for lifecycles that wrap non-XML or
     * non-rebuildable widgets. Concrete panel/overlay bases override to rebuild.
     *
     * @return true if rebuilt, false if skipped (not yet shown, not this instance, etc.)
     */
    virtual bool rebuild() {
        return false;
    }

    /**
     * @brief Get human-readable name for logging
     * @return Panel/overlay name (e.g., "Motion Panel", "Network Settings")
     */
    virtual const char* get_name() const = 0;
};

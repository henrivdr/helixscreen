// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "wifi_backend.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief WiFi Manager - Clean interface using backend system
 *
 * Provides network scanning, connection management, and status monitoring.
 * Uses pluggable backend system:
 * - Linux: WifiBackendWpaSupplicant for real wpa_supplicant integration
 * - macOS: WifiBackendMock for simulator testing
 *
 * Key improvements over old implementation:
 * - No platform ifdefs in manager code
 * - Event-driven architecture with proper callbacks
 * - Thread-safe communication between backend and UI
 * - Cleaner separation between WiFi operations and UI timer management
 */
class WiFiManager {
  public:
    /**
     * @brief Initialize WiFi manager with appropriate backend
     *
     * Automatically selects platform-appropriate backend and starts it.
     *
     * @param silent If true, suppress error modals on startup (used when WiFi
     *               wasn't previously configured and we're just probing availability)
     */
    explicit WiFiManager(bool silent = false);

    /**
     * @brief Destructor - ensures clean shutdown
     */
    ~WiFiManager();

    // ========================================================================
    // Network Scanning
    // ========================================================================

    /**
     * @brief Perform a single network scan (synchronous)
     *
     * Triggers scan and returns results immediately.
     * Uses backend's get_scan_results() after triggering scan.
     *
     * @return Vector of discovered WiFi networks
     */
    std::vector<WiFiNetwork> scan_once();

    /**
     * @brief Start periodic network scanning
     *
     * Scans for available networks and invokes callback with results.
     * Scanning continues automatically every 7 seconds until stop_scan() called.
     *
     * @param on_networks_updated Callback invoked with scan results
     */
    void start_scan(std::function<void(const std::vector<WiFiNetwork>&)> on_networks_updated);

    /**
     * @brief Stop periodic network scanning
     *
     * Cancels auto-refresh timer and any pending scan operations.
     */
    void stop_scan();

    // ========================================================================
    // Connection Management
    // ========================================================================

    /**
     * @brief Connect to WiFi network
     *
     * Attempts to connect to the specified network. Operation is asynchronous;
     * callback invoked when connection succeeds or fails.
     *
     * @param ssid Network name
     * @param password Network password (empty for open networks)
     * @param on_complete Callback with (success, error_message)
     */
    void connect(const std::string& ssid, const std::string& password,
                 std::function<void(bool success, const std::string& error)> on_complete);

    /**
     * @brief Disconnect from current network
     */
    void disconnect();

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * @brief Check if connected to any network
     *
     * @return true if connected
     */
    bool is_connected();

    /**
     * @brief Get currently connected network name
     *
     * @return SSID of connected network, or empty string if not connected
     */
    std::string get_connected_ssid();

    /**
     * @brief Get current IP address
     *
     * @return IP address string (e.g., "192.168.1.100"), or empty if not connected
     */
    std::string get_ip_address();

    /**
     * @brief Get WiFi adapter MAC address
     *
     * @return MAC address string (e.g., "aa:bb:cc:dd:ee:ff"), or empty if unavailable
     */
    std::string get_mac_address();

    /**
     * @brief Get signal strength of connected network
     *
     * @return Signal strength 0-100%, or 0 if not connected
     */
    int get_signal_strength();

    /**
     * @brief Check if WiFi hardware supports 5GHz band
     *
     * Returns true if the WiFi adapter can connect to 5GHz networks.
     * Used to conditionally show "Only 2.4GHz networks" in the UI.
     *
     * @return true if 5GHz is supported, false if only 2.4GHz
     */
    bool supports_5ghz();

    // ========================================================================
    // Hardware Detection (Legacy Compatibility)
    // ========================================================================

    /**
     * @brief Check if WiFi hardware is available
     *
     * Always returns true - backend creation handles hardware availability.
     * Kept for compatibility with existing UI code.
     *
     * @return true if WiFi backend is available
     */
    bool has_hardware();

    /**
     * @brief Check if WiFi is currently enabled
     *
     * @return true if backend is running
     */
    bool is_enabled();

    /**
     * @brief Enable or disable WiFi radio
     *
     * @param enabled true to enable, false to disable
     * @return true on success
     */
    bool set_enabled(bool enabled);

    /**
     * @brief Non-blocking re-attempt of backend bringup.
     *
     * The backend's initial start_async() (kicked from the constructor) can fail
     * on a fresh boot if the system WiFi service (wpa_supplicant) hasn't created
     * its control socket yet. Without a retry the backend stays permanently dead
     * and every scan reports "Backend not started" (helixscreen#1036). This kicks
     * a fresh start_async() on a worker thread; on success the backend fires
     * READY, which wakes state observers and the periodic scan. Safe to call
     * repeatedly — concurrent attempts are de-duplicated by the backend, and a
     * call while already connected is a cheap no-op (re-fires READY).
     */
    void retry_async();

    /**
     * @brief Subscribe to backend state changes (READY / CONNECTED / DISCONNECTED)
     *
     * Fires when the backend's connection state transitions — including the
     * initial READY event that lands after async init completes. The callback
     * is marshalled to the UI thread via the caller's LifetimeToken and is
     * silently skipped if the token has expired by the time it runs.
     *
     * Use this to react to state changes without polling (e.g. to unstick UI
     * queried before async backend init landed).
     *
     * @param token   Caller's LifetimeToken — expires with the owning object
     * @param on_change Callback invoked on the UI thread after state changes
     */
    void add_state_observer(helix::LifetimeToken token, std::function<void()> on_change);

    /**
     * @brief Initialize self-reference for async callback safety
     *
     * MUST be called immediately after construction when using shared_ptr.
     * Enables async callbacks to safely check if manager still exists.
     *
     * @param self Shared pointer to this WiFiManager instance
     */
    void init_self_reference(std::shared_ptr<WiFiManager> self);

  private:
    // Grants the auth-failure-debounce regression test direct access to the
    // connection handlers and grace-timer state (helixscreen#1050).
    friend class WiFiManagerTestAccess;

    std::unique_ptr<WifiBackend> backend_;

    // Self-reference for async callback safety
    // Weak pointers in async callbacks can safely check if manager still exists
    std::shared_ptr<WiFiManager> self_;

    // Guards the callback/flag state below, which is read on the libhv backend
    // thread (handle_scan_complete / handle_connected / handle_disconnected /
    // handle_auth_failed) and written on the main/LVGL thread (start_scan /
    // stop_scan / connect / scan_timer_callback / deliver_auth_failure). Reading
    // or reassigning a std::function concurrently is a data race (UB). Callbacks
    // are copied under the lock and invoked OUTSIDE it to avoid re-entrant
    // deadlock and holding the lock across arbitrary UI code.
    mutable std::mutex callback_mutex_;

    // Scanning state
    lv_timer_t* scan_timer_;
    std::function<void(const std::vector<WiFiNetwork>&)> scan_callback_; // guarded by callback_mutex_
    bool scan_pending_; // guarded by callback_mutex_; true when scan triggered, cleared after first
                        // SCAN_COMPLETE processed

    // Connection state
    std::function<void(bool, const std::string&)> connect_callback_; // guarded by callback_mutex_
    bool connecting_in_progress_ = false; // guarded by callback_mutex_; true during connect attempt,
                                          // prevents false failure on DISCONNECTED

    // Auth-failure debounce (helixscreen#1050). Some adapters' wpa_supplicant emit a
    // transient CTRL-EVENT-SSID-TEMP-DISABLED/WRONG_KEY mid-handshake on a connect that
    // ultimately succeeds (CONNECTED follows ~1-3s later). Treating that AUTH_FAILED as
    // terminal latched failure into the wizard while WiFi was actually up. Instead the
    // failure is deferred for a grace window; a CONNECTED arriving within it preempts and
    // delivers success. Only a real wrong password (no CONNECTED) surfaces the error.
    // Touched on the UI thread only (the timer and the queue_update apply lambdas).
    lv_timer_t* auth_fail_grace_timer_ = nullptr;
    std::string pending_auth_error_;
    void start_auth_fail_grace(const std::string& error); // arm/restart grace window
    void cancel_auth_fail_grace();                        // CONNECTED preempted the failure
    void deliver_auth_failure();                          // grace elapsed — failure is real
    static void auth_fail_grace_timer_cb(lv_timer_t* timer);

    // Event handling
    void handle_scan_complete(const std::string& event_data);
    void handle_connected(const std::string& event_data);
    void handle_disconnected(const std::string& event_data);
    void handle_auth_failed(const std::string& event_data);
    void handle_init_failed(bool silent, const std::string& msg);

    // Registers SCAN_COMPLETE/CONNECTED/DISCONNECTED/AUTH_FAILED/INIT_FAILED/READY
    // handlers on the current backend_. Called once from the constructor and
    // again after swapping backends during INIT_FAILED auto-failover.
    void register_backend_callbacks(bool silent);

    // On Linux, WiFiManager attempts a one-shot NetworkManager -> wpa_supplicant
    // fallback when NM's INIT_FAILED fires (daemon dead despite nmcli present).
    // This flag prevents infinite loops if wpa_supplicant also fails.
    bool tried_fallback_ = false;

    // Observers of backend state transitions — each entry is a {token, cb} pair.
    // Fans out READY/CONNECTED/DISCONNECTED so UI consumers (home-panel network
    // widget) can unstick themselves when they queried before async init landed.
    struct StateObserver {
        helix::LifetimeToken token;
        std::function<void()> callback;
    };
    std::mutex state_observers_mutex_;
    std::vector<StateObserver> state_observers_;
    void notify_state_observers();

    // Timer callbacks (must be static for LVGL)
    static void scan_timer_callback(lv_timer_t* timer);

    // True when the OS reports a live wireless link even though the managed
    // backend (wpa_supplicant) is unreachable. In that state the link is
    // genuinely up (printer reachable by IP, no managed control), so the
    // "scan failed" / "service unavailable" warnings are demoted to debug logs
    // rather than nagging the user (helixscreen#1059, Qidi Q2). Defaults to the
    // real sysfs/proc probe; tests inject a stub via WiFiManagerTestAccess.
    static std::function<bool()> os_link_probe_;
    static bool os_link_up();
};

/**
 * @brief Get the global WiFiManager instance
 *
 * Returns a lazily-created singleton WiFiManager. Use this from all
 * components (wizard, home panel, etc.) rather than creating instances.
 *
 * @return Shared pointer to the global WiFiManager instance
 */
std::shared_ptr<WiFiManager> get_wifi_manager();

} // namespace helix

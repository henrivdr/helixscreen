// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file wifi_manager.cpp
 * @brief High-level WiFi operations manager wrapping platform backends
 *
 * @pattern Manager with weak self_ reference for callback safety
 * @threading Backend callbacks may run on background thread
 * @gotchas Uses fprintf in destructor instead of spdlog; clears callbacks BEFORE stopping backend
 *
 * @see wifi_backend.cpp
 */

#include "wifi_manager.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"
#include "safe_log.h"
#include "spdlog/spdlog.h"
#include "wifi_ui_utils.h"

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

using namespace helix;

// Default OS-level link probe: true when the kernel reports a wireless iface
// with an up link. Overridable in tests via WiFiManagerTestAccess.
std::function<bool()> WiFiManager::os_link_probe_ = []() {
    return helix::ui::wifi::probe_os_wifi_link().has_link;
};

bool WiFiManager::os_link_up() {
    return os_link_probe_ && os_link_probe_();
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

WiFiManager::WiFiManager(bool silent) : scan_timer_(nullptr), scan_pending_(false) {
    spdlog::debug("[WiFiManager] Initializing with backend system{}",
                  silent ? " (silent mode)" : "");

    // Create platform-appropriate backend. Factory returns immediately —
    // backend is NOT yet initialized. We register our event handlers first,
    // then kick off the deferred init via start_async() so we never miss a
    // READY / INIT_FAILED event.
    backend_ = WifiBackend::create(silent);
    if (!backend_) {
        if (!silent) {
            NOTIFY_ERROR_MODAL("WiFi Unavailable",
                               "Could not initialize WiFi hardware. Check system configuration.");
        } else {
            spdlog::debug("[WiFiManager] WiFi unavailable (silent mode - no modal)");
        }
        return;
    }

    // Register event callbacks BEFORE kicking off async init
    register_backend_callbacks(silent);

    // Kick off deferred initialization on a worker thread — this returns
    // immediately so the UI thread isn't blocked on subprocess probing.
    backend_->start_async();
}

void WiFiManager::register_backend_callbacks(bool silent) {
    backend_->register_event_callback(
        "SCAN_COMPLETE", [this](const std::string& data) { handle_scan_complete(data); });
    backend_->register_event_callback("CONNECTED",
                                      [this](const std::string& data) { handle_connected(data); });
    backend_->register_event_callback(
        "DISCONNECTED", [this](const std::string& data) { handle_disconnected(data); });
    backend_->register_event_callback(
        "AUTH_FAILED", [this](const std::string& data) { handle_auth_failed(data); });
    backend_->register_event_callback(
        "INIT_FAILED", [this, silent](const std::string& msg) { handle_init_failed(silent, msg); });
    backend_->register_event_callback("READY", [this](const std::string&) {
        spdlog::debug("[WiFiManager] Backend READY event received");
        // Wake UI consumers that queried status before async init landed —
        // NetworkWidget in particular attaches synchronously during home-panel
        // load, races the backend's worker thread, and gets an empty STATUS
        // response that pins it on 'Disconnected' until this event lands.
        notify_state_observers();
    });
}

void WiFiManager::handle_init_failed(bool silent, const std::string& msg) {
    // On Linux, if the NetworkManager backend fails (e.g. nmcli binary present
    // but NM daemon masked/dead), transparently fall back to wpa_supplicant
    // so users aren't left WiFi-less because of a dormant NM install. Guarded
    // by tried_fallback_ to avoid infinite loops if wpa_supplicant also fails.
#if !defined(__APPLE__) && !defined(__ANDROID__)
    if (!tried_fallback_ && backend_ &&
        dynamic_cast<WifiBackendNetworkManager*>(backend_.get()) != nullptr) {
        tried_fallback_ = true;
        spdlog::warn("[WiFiManager] NetworkManager backend INIT_FAILED ({}); "
                     "falling back to wpa_supplicant",
                     msg);
        // CRITICAL: INIT_FAILED fires from inside the NM backend's init worker
        // thread. Calling backend_->stop() here would invoke
        // init_thread_.join() on the currently-executing thread, producing
        // std::system_error(resource_deadlock_would_occur). Defer the swap to
        // the main/UI thread via UpdateQueue so the init thread can unwind
        // before stop() joins it. WiFiManager is a process singleton owned by
        // g_shared_wifi_manager, so capturing `this` is safe.
        helix::ui::queue_update("WiFiManager::fallback_to_wpa_supplicant", [this, silent]() {
            if (!backend_) {
                return;
            }
            backend_->stop();
            backend_.reset();
            backend_ = std::make_unique<WifiBackendWpaSupplicant>();
            backend_->set_silent(silent);
            register_backend_callbacks(silent);
            backend_->start_async();
        });
        return;
    }
#endif
    // Backend initialization failed asynchronously - notify user (unless silent)
    if (!silent) {
        NOTIFY_ERROR("WiFi initialization failed: {}", msg);
    } else {
        spdlog::debug("[WiFiManager] WiFi init failed (silent): {}", msg);
    }
}

void WiFiManager::init_self_reference(std::shared_ptr<WiFiManager> self) {
    self_ = self;
    spdlog::debug("[WiFiManager] Self-reference initialized for async callback safety");
}

WiFiManager::~WiFiManager() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WiFiManager] Destructor called\n");

    // Clean up scanning
    stop_scan();

    // Clean up any pending auth-failure grace timer (helixscreen#1050)
    if (auth_fail_grace_timer_ && lv_is_initialized()) {
        lv_timer_delete(auth_fail_grace_timer_);
    }
    auth_fail_grace_timer_ = nullptr;

    // Clear callbacks BEFORE stopping backend
    // Pending lv_async_call operations check for null callbacks before invoking
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = nullptr;
        connect_callback_ = nullptr;
    }

    // Stop backend (this stops backend threads)
    if (backend_) {
        backend_->stop();
    }
}

// ============================================================================
// Network Scanning
// ============================================================================

std::vector<WiFiNetwork> WiFiManager::scan_once() {
    if (!backend_) {
        LOG_WARN_INTERNAL("No backend available for scan");
        return {};
    }

    spdlog::debug("[WiFiManager] Performing single scan");

    // Trigger scan and wait briefly for results
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        LOG_WARN_INTERNAL("Failed to trigger scan: {}", scan_result.technical_msg);
        return {};
    }

    // For synchronous scan, we need to get existing results
    // Note: This may not include the just-triggered scan results immediately
    std::vector<WiFiNetwork> networks;
    WiFiError get_result = backend_->get_scan_results(networks);
    if (!get_result.success()) {
        LOG_WARN_INTERNAL("Failed to get scan results: {}", get_result.technical_msg);
        return {};
    }

    return networks;
}

void WiFiManager::start_scan(
    std::function<void(const std::vector<WiFiNetwork>&)> on_networks_updated) {
    if (!backend_) {
        NOTIFY_ERROR("WiFi unavailable. Cannot scan for networks.");
        return;
    }

    spdlog::debug("[WiFiManager] start_scan ENTRY, callback is {}",
                  on_networks_updated ? "NOT NULL" : "NULL");

    // Stop existing timer if running (also clears old callback)
    stop_scan();

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = on_networks_updated;
    }
    spdlog::debug("[WiFiManager] Scan callback registered");

    spdlog::info("[WiFiManager] Starting periodic network scan (every 7 seconds)");

    // Create timer for periodic scanning
    scan_timer_ = lv_timer_create(scan_timer_callback, 7000, this);
    spdlog::debug("[WiFiManager] Timer created: {}", (void*)scan_timer_);

    // Trigger immediate scan
    spdlog::debug("[WiFiManager] About to trigger initial scan");
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_pending_ = true; // Mark scan as pending - cleared after first SCAN_COMPLETE processed
    }
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            scan_pending_ = false;
        }
        // If the OS reports the wireless link is actually up, the managed
        // backend simply can't reach its control socket (the link is system-
        // managed and live). Nagging the user with a failure toast is wrong —
        // demote to a debug log (helixscreen#1059).
        if (os_link_up()) {
            spdlog::debug("[WiFiManager] Scan trigger failed but OS link is up "
                          "(system-managed) — suppressing user warning");
        } else {
            NOTIFY_WARNING("WiFi scan failed. Try again.");
        }
    } else {
        spdlog::debug("[WiFiManager] Initial scan triggered successfully");
    }
}

void WiFiManager::stop_scan() {
    if (scan_timer_ && lv_is_initialized()) {
        lv_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
        spdlog::info("[WiFiManager] Stopped network scanning");
    }
    // Clear callback to prevent stale callbacks firing after the caller
    // deactivates/destroys (the callback captures a raw overlay pointer).
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = nullptr;
    }
}

void WiFiManager::scan_timer_callback(lv_timer_t* timer) {
    WiFiManager* manager = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (manager && manager->backend_) {
        // Trigger scan - results will arrive via SCAN_COMPLETE event
        {
            std::lock_guard<std::mutex> lock(manager->callback_mutex_);
            manager->scan_pending_ = true; // Mark scan as pending
        }
        WiFiError result = manager->backend_->trigger_scan();
        if (!result.success()) {
            {
                std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                manager->scan_pending_ = false;
            }
            LOG_WARN_INTERNAL("Periodic scan failed: {}", result.technical_msg);
            // Self-heal: a NOT_INITIALIZED scan means the backend never came up
            // (typically a fresh-boot race where wpa_supplicant's control socket
            // wasn't ready when the constructor first probed). Re-attempt bringup
            // here so the backend recovers on its own once the socket appears,
            // instead of looping "Backend not started" forever (helixscreen#1036).
            if (result.result == WiFiResult::NOT_INITIALIZED) {
                spdlog::info("[WiFiManager] Backend not started — re-attempting bringup");
                manager->backend_->start_async();
            }
        }
    }
}

// ============================================================================
// Connection Management
// ============================================================================

void WiFiManager::connect(const std::string& ssid, const std::string& password,
                          std::function<void(bool success, const std::string& error)> on_complete) {
    if (!backend_) {
        NOTIFY_ERROR("WiFi unavailable. Cannot connect to network.");
        if (on_complete) {
            on_complete(false, "No WiFi backend available");
        }
        return;
    }

    spdlog::info("[WiFiManager] Connecting to '{}'", ssid);

    // Drop any grace timer left over from a prior attempt so it can't deliver a stale
    // failure against this new connect (helixscreen#1050).
    cancel_auth_fail_grace();

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connect_callback_ = on_complete;
        connecting_in_progress_ = true; // Ignore DISCONNECTED events during connection attempt
    }
    spdlog::debug("[WiFiManager] Connect callback registered for '{}'", ssid);

    // Use backend's connect method
    WiFiError result = backend_->connect_network(ssid, password);
    if (!result.success()) {
        NOTIFY_ERROR("Failed to connect to WiFi network '{}'", ssid);
        // Clear in-progress + take the callback under the lock, then invoke the
        // local copy OUTSIDE the lock (the callback may re-enter WiFiManager).
        std::function<void(bool, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            connecting_in_progress_ = false; // Clear on sync failure
            cb = std::move(connect_callback_);
            connect_callback_ = nullptr;
        }
        if (cb) {
            cb(false, result.user_msg.empty() ? result.technical_msg : result.user_msg);
        }
    }
    // Success/failure will be reported via CONNECTED/AUTH_FAILED events
}

void WiFiManager::disconnect() {
    if (!backend_) {
        LOG_WARN_INTERNAL("No backend available for disconnect");
        return;
    }

    spdlog::info("[WiFiManager] Disconnecting");
    WiFiError result = backend_->disconnect_network();
    if (!result.success()) {
        NOTIFY_WARNING("Could not disconnect from WiFi");
    }
}

// ============================================================================
// Status Queries
// ============================================================================

bool WiFiManager::is_connected() {
    if (!backend_)
        return false;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.connected;
}

std::string WiFiManager::get_connected_ssid() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ssid;
}

std::string WiFiManager::get_ip_address() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ip_address;
}

std::string WiFiManager::get_mac_address() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.mac_address;
}

int WiFiManager::get_signal_strength() {
    if (!backend_)
        return 0;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.signal_strength;
}

bool WiFiManager::supports_5ghz() {
    if (!backend_)
        return false;

    return backend_->supports_5ghz();
}

// ============================================================================
// Hardware Detection (Legacy Compatibility)
// ============================================================================

bool WiFiManager::has_hardware() {
    // Backend creation handles hardware availability
    return (backend_ != nullptr);
}

bool WiFiManager::is_enabled() {
    if (!backend_)
        return false;
    return backend_->is_running();
}

bool WiFiManager::set_enabled(bool enabled) {
    if (!backend_)
        return false;

    spdlog::debug("[WiFiManager] set_enabled({})", enabled);

    if (enabled) {
        // Explicit user toggle — synchronous start() is acceptable here
        // (user already gated on the click; brief subprocess probing is OK).
        // The non-blocking path lives in the constructor so first-paint
        // isn't stalled.
        WiFiError result = backend_->start();
        if (!result.success()) {
            // A live system-managed link (printer reachable by IP) means the
            // backend just can't reach wpa_supplicant's control socket; the
            // radio is not actually off. Don't surface a hard error toast for
            // an unmanageable-but-up link (helixscreen#1059).
            if (os_link_up()) {
                spdlog::debug("[WiFiManager] Backend start failed but OS link is up "
                              "(system-managed) — suppressing user error: {}",
                              result.user_msg.empty() ? result.technical_msg : result.user_msg);
            } else {
                NOTIFY_ERROR("Failed to enable WiFi: {}",
                             result.user_msg.empty() ? result.technical_msg : result.user_msg);
            }
        } else {
            spdlog::debug("[WiFiManager] WiFi backend started successfully");
        }
        return result.success();
    } else {
        backend_->stop();
        spdlog::debug("[WiFiManager] WiFi backend stopped");
        return true;
    }
}

void WiFiManager::retry_async() {
    if (!backend_) {
        return;
    }
    spdlog::debug("[WiFiManager] Re-attempting backend bringup (retry_async)");
    backend_->start_async();
}

// ============================================================================
// Event Handling
// ============================================================================

// Helper struct for async callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ScanCallbackData {
    std::weak_ptr<WiFiManager> manager;
    std::vector<WiFiNetwork> networks;
};

void WiFiManager::handle_scan_complete(const std::string& event_data) {
    (void)event_data; // Unused for now

    spdlog::debug("[WiFiManager] handle_scan_complete ENTRY (backend thread)");

    // Debounce: wpa_supplicant can emit duplicate SCAN_RESULTS events
    // Only process the first one per scan cycle. This runs on the backend thread,
    // so the flag/callback reads are serialized against the main thread via
    // callback_mutex_.
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (!scan_pending_) {
            spdlog::trace("[WiFiManager] Ignoring duplicate SCAN_COMPLETE (already processed)");
            return;
        }
        scan_pending_ = false; // Clear flag - subsequent events for this scan will be ignored

        if (!scan_callback_) {
            LOG_WARN_INTERNAL("Scan complete but no callback registered");
            return;
        }
    }

    // CRITICAL: This is called from backend thread - must dispatch to LVGL thread!
    spdlog::debug("[WiFiManager] Scan callback is registered, fetching results");
    std::vector<WiFiNetwork> networks;
    WiFiError result = backend_->get_scan_results(networks);

    if (result.success()) {
        spdlog::debug("[WiFiManager] Got {} scan results, dispatching to LVGL thread",
                      networks.size());

        // Use RAII-safe async callback wrapper
        helix::ui::queue_update<ScanCallbackData>(
            std::make_unique<ScanCallbackData>(ScanCallbackData{self_, networks}),
            [](ScanCallbackData* data) {
                spdlog::debug("[WiFiManager] async_call executing in LVGL thread with {} networks",
                              data->networks.size());

                // Safely check if manager still exists
                if (auto manager = data->manager.lock()) {
                    std::function<void(const std::vector<WiFiNetwork>&)> cb;
                    {
                        std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                        cb = manager->scan_callback_;
                    }
                    if (cb) {
                        cb(data->networks);
                        spdlog::debug("[WiFiManager] scan_callback_ completed successfully");
                    } else {
                        spdlog::warn(
                            "[WiFiManager] scan_callback_ was cleared before async dispatch");
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }
            });

    } else {
        LOG_WARN_INTERNAL("Failed to get scan results: {}", result.technical_msg);

        // Use RAII-safe async callback wrapper
        helix::ui::queue_update<ScanCallbackData>(
            std::make_unique<ScanCallbackData>(ScanCallbackData{self_, {}}),
            [](ScanCallbackData* data) {
                LOG_WARN_INTERNAL("async_call: calling callback with empty results");
                if (auto manager = data->manager.lock()) {
                    std::function<void(const std::vector<WiFiNetwork>&)> cb;
                    {
                        std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                        cb = manager->scan_callback_;
                    }
                    if (cb) {
                        cb({});
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }
            });
    }

    spdlog::debug("[WiFiManager] handle_scan_complete EXIT (dispatch queued)");
}

// Helper struct for connection callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ConnectCallbackData {
    std::weak_ptr<WiFiManager> manager;
    bool success;
    std::string error;
};

void WiFiManager::handle_connected(const std::string& event_data) {
    (void)event_data; // Could parse IP address from event data

    spdlog::debug("[WiFiManager] Connected event received (backend thread)");

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false; // Connection complete
    }

    // Fan out to passive UI observers regardless of whether there's an active
    // connect() callback — the home-panel network widget depends on this to
    // learn the initial post-boot connection state.
    notify_state_observers();

    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        spdlog::debug(
            "[WiFiManager] Connected event but no callback registered (normal on startup)");
        return;
    }

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(ConnectCallbackData{self_, true, ""}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                // A transient AUTH_FAILED may be sitting in the grace window — this
                // CONNECTED is the real outcome, so cancel it before delivering success
                // (helixscreen#1050).
                manager->cancel_auth_fail_grace();
                std::function<void(bool, const std::string&)> cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before connect callback - safely ignored");
            }
        });
}

void WiFiManager::handle_disconnected(const std::string& event_data) {
    (void)event_data; // Could parse reason from event data

    spdlog::debug("[WiFiManager] Disconnected event received (backend thread)");

    // During a connection attempt, wpa_supplicant fires DISCONNECTED before CONNECTED
    // when switching networks. Ignore DISCONNECTED during connection - only AUTH_FAILED
    // or subsequent CONNECTED should determine success/failure.
    bool in_progress;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        in_progress = connecting_in_progress_;
    }
    if (in_progress) {
        spdlog::debug("[WiFiManager] Ignoring DISCONNECTED during connection attempt");
        return;
    }

    // Genuine disconnect — wake passive observers so they can refresh UI state.
    notify_state_observers();

    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        spdlog::debug("[WiFiManager] Disconnected event but no callback registered (normal)");
        return;
    }

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(ConnectCallbackData{self_, false, "Disconnected"}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                std::function<void(bool, const std::string&)> cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before disconnect callback - safely ignored");
            }
        });
}

void WiFiManager::handle_auth_failed(const std::string& event_data) {
    spdlog::warn("[WiFiManager] Authentication failed event received (backend thread)");

    // Do NOT clear connecting_in_progress_ or deliver the failure synchronously here.
    // wpa_supplicant emits a transient CTRL-EVENT-SSID-TEMP-DISABLED/WRONG_KEY mid-handshake
    // on some adapters even when the connect ultimately succeeds — a CONNECTED follows ~1-3s
    // later (helixscreen#1050). Staying "in progress" keeps any interleaved DISCONNECTED
    // ignored, and the failure is armed on a grace timer that a CONNECTED can preempt.
    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        LOG_WARN_INTERNAL("Auth failed event but no callback registered");
        return;
    }

    // Pass through backend detail if provided, otherwise generic message
    std::string error_msg = event_data.empty() ? "Authentication failed" : event_data;

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(
            ConnectCallbackData{self_, false, std::move(error_msg)}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                // Only arm the grace window if a connect is still pending; a CONNECTED that
                // already resolved it nulls connect_callback_.
                bool still_pending;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    still_pending = static_cast<bool>(manager->connect_callback_);
                }
                if (still_pending) {
                    manager->start_auth_fail_grace(d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before auth_failed callback - safely ignored");
            }
        });
}

// ----------------------------------------------------------------------------
// Auth-failure debounce (helixscreen#1050) — UI thread only
// ----------------------------------------------------------------------------

namespace {
// Window to wait after a transient AUTH_FAILED for a CONNECTED to arrive. wpa_supplicant's
// TEMP-DISABLED -> CONNECTED gap is typically 1-3s; allow margin for slow embedded adapters.
// A genuine wrong password (no CONNECTED) surfaces the failure after this delay.
constexpr uint32_t AUTH_FAIL_GRACE_MS = 4000;
} // namespace

void WiFiManager::start_auth_fail_grace(const std::string& error) {
    pending_auth_error_ = error;
    if (auth_fail_grace_timer_) {
        lv_timer_delete(auth_fail_grace_timer_);
        auth_fail_grace_timer_ = nullptr;
    }
    auth_fail_grace_timer_ = lv_timer_create(auth_fail_grace_timer_cb, AUTH_FAIL_GRACE_MS, this);
    lv_timer_set_repeat_count(auth_fail_grace_timer_, 1); // one-shot
    spdlog::debug("[WiFiManager] AUTH_FAILED deferred {}ms pending possible CONNECTED",
                  AUTH_FAIL_GRACE_MS);
}

void WiFiManager::cancel_auth_fail_grace() {
    if (auth_fail_grace_timer_) {
        spdlog::debug("[WiFiManager] CONNECTED preempted pending AUTH_FAILED — transient "
                      "handshake failure ignored");
        lv_timer_delete(auth_fail_grace_timer_);
        auth_fail_grace_timer_ = nullptr;
    }
    pending_auth_error_.clear();
}

void WiFiManager::deliver_auth_failure() {
    // Grace window elapsed with no CONNECTED — the authentication failure is real.
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false;
    }
    notify_state_observers();
    std::function<void(bool, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = std::move(connect_callback_);
        connect_callback_ = nullptr;
    }
    if (cb) {
        cb(false, pending_auth_error_);
    }
    pending_auth_error_.clear();
}

void WiFiManager::auth_fail_grace_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // One-shot: LVGL deletes the timer after this callback returns, so just drop our handle.
    self->auth_fail_grace_timer_ = nullptr;
    self->deliver_auth_failure();
}

// ============================================================================
// State Observers
// ============================================================================

void WiFiManager::add_state_observer(helix::LifetimeToken token, std::function<void()> on_change) {
    if (!on_change) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_observers_mutex_);
    state_observers_.push_back({std::move(token), std::move(on_change)});
}

void WiFiManager::notify_state_observers() {
    // Snapshot under lock, then invoke outside the lock so defer() — and any
    // work it kicks off on the UI thread — can't call back into us while we
    // hold state_observers_mutex_. Drop expired entries as we go; the backend
    // callback threads are where this runs, so a bit of cleanup here is fine.
    std::vector<StateObserver> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_observers_mutex_);
        state_observers_.erase(
            std::remove_if(state_observers_.begin(), state_observers_.end(),
                           [](const StateObserver& obs) { return obs.token.expired(); }),
            state_observers_.end());
        snapshot = state_observers_;
    }
    for (const auto& obs : snapshot) {
        obs.token.defer("WiFiManager::state_observer", obs.callback);
    }
}

// ============================================================================
// Shared Singleton Instance
// ============================================================================

// Global shared WiFiManager instance
// Using static local ensures thread-safe lazy initialization (C++11 guarantee)
static std::shared_ptr<WiFiManager> g_shared_wifi_manager;
static std::mutex g_wifi_manager_mutex;

namespace helix {

std::shared_ptr<WiFiManager> get_wifi_manager() {
    std::lock_guard<std::mutex> lock(g_wifi_manager_mutex);

    if (!g_shared_wifi_manager) {
        spdlog::debug("[WiFiManager] Creating global instance");
        // Use silent=true for global instance since it's used for passive status monitoring
        // (e.g., home panel WiFi icon). Avoids modal popup when WiFi hardware is unavailable
        // on development machines or when WiFi is simply turned off.
        g_shared_wifi_manager = std::make_shared<WiFiManager>(/*silent=*/true);
        g_shared_wifi_manager->init_self_reference(g_shared_wifi_manager);
    }

    return g_shared_wifi_manager;
}

} // namespace helix

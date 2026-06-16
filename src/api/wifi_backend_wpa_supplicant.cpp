// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend_wpa_supplicant.h"

#include "ui_error_reporting.h"

#include "spdlog/spdlog.h"
#include "wifi_5ghz_detection.h"

#if !defined(__APPLE__) && !defined(__ANDROID__)
// ============================================================================
// Linux Implementation: Full wpa_supplicant integration
// ============================================================================

#include "wpa_ctrl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

/// Resolve a client socket directory visible outside systemd's PrivateTmp.
///
/// wpa_ctrl_open() binds a UNIX datagram reply socket in /tmp by default.
/// Under PrivateTmp=true the real /tmp is a private mount — wpa_supplicant
/// (running outside the namespace) cannot sendto() the reply address, so
/// wpa_ctrl_attach() blocks until timeout.
///
/// We prefer /run/helixscreen (created by RuntimeDirectory=helixscreen in the
/// service unit).  Falls back to /tmp for manual/non-systemd launches.
static const char* resolve_wpa_client_dir() {
    // RuntimeDirectory is the ideal location — visible to all processes
    if (fs::is_directory("/run/helixscreen"))
        return "/run/helixscreen";
    return nullptr; // nullptr → wpa_ctrl_open() uses its /tmp default
}

/// Normalise a wpa_supplicant control-interface value to a plain directory.
///
/// The `-O` / `ctrl_interface` value is either a bare directory
/// (e.g. Creality's `/etc/wifi/wpa_supplicant/sockets`) or the keyed form
/// `DIR=<path> GROUP=<grp>` (the standard systemd launch).  Return the <path>.
static std::string parse_wpa_ctrl_value(std::string v) {
    const std::string key = "DIR=";
    const auto pos = v.find(key);
    if (pos != std::string::npos)
        v.erase(0, pos + key.size());
    // Drop a trailing " GROUP=..." (or any other whitespace-separated token).
    const auto ws = v.find_first_of(" \t");
    if (ws != std::string::npos)
        v.erase(ws);
    return v;
}

/// Parse a wpa_supplicant config file's `ctrl_interface=` directive.
///
/// Vendor init scripts frequently launch `wpa_supplicant -c <conf>` with a
/// non-standard ctrl_interface set in the config file rather than passing
/// -O/-C on the command line, so scanning the cmdline alone would miss them.
/// The value uses the same bare-dir or `DIR=<path> GROUP=<grp>` form as -O.
/// Returns "" if the file is unreadable, has no ctrl_interface, or the value
/// is not an absolute directory path.
namespace helix::wifi::detail {
std::string read_ctrl_interface_from_conf(const std::string& conf_path) {
    std::ifstream conf(conf_path);
    if (!conf.is_open())
        return {};

    const std::string key = "ctrl_interface=";
    std::string line;
    while (std::getline(conf, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue; // blank line or comment
        if (line.compare(first, key.size(), key) != 0)
            continue;

        std::string val = line.substr(first + key.size());
        // Trim trailing whitespace / CR (CRLF configs).
        const auto last = val.find_last_not_of(" \t\r\n");
        if (last != std::string::npos)
            val.erase(last + 1);

        std::string dir = parse_wpa_ctrl_value(std::move(val));
        // ctrl_interface may also be a DBus-style name; only accept it when it
        // resolves to an absolute filesystem directory path.
        if (!dir.empty() && dir.front() == '/')
            return dir;
    }
    return {};
}
} // namespace helix::wifi::detail

/// Inspect running processes for a `wpa_supplicant` daemon and return the
/// control-interface directory it was launched with.
///
/// The directory is whatever wpa_supplicant was launched with; some vendor
/// firmwares relocate it off the standard paths.  Auto-detecting it from the
/// live process lets us adapt without per-device patches or manual symlinks.
/// Precedence mirrors wpa_supplicant itself: `-O <dir>` overrides the control
/// directory; otherwise the `ctrl_interface` in the `-c <conf>` config file;
/// otherwise a `-C <ctrl>` value (used only when no -c is given).
/// Returns "" when not found (no daemon, no usable arg, or /proc unavailable).
static std::string detect_wpa_ctrl_dir_from_proc() {
    std::error_code ec;
    if (!fs::is_directory("/proc", ec))
        return {};

    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec)
            break;
        // PID directories only.
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;

        // /proc/<pid>/cmdline is NUL-separated argv.
        std::ifstream cmd(entry.path() / "cmdline", std::ios::binary);
        if (!cmd.is_open())
            continue;
        std::vector<std::string> argv;
        std::string arg;
        while (std::getline(cmd, arg, '\0'))
            argv.push_back(arg);
        if (argv.empty())
            continue;

        // argv[0] basename must be wpa_supplicant.
        if (fs::path(argv[0]).filename().string() != "wpa_supplicant")
            continue;

        // Collect the relevant flags. Each accepts a separate "-X value" or a
        // joined "-Xvalue" form. -O / -C values may be the keyed
        // "DIR=<path> GROUP=<grp>" form (parse_wpa_ctrl_value handles both).
        std::string o_dir, c_val, conf_path;
        for (size_t i = 1; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a == "-O" && i + 1 < argv.size())
                o_dir = parse_wpa_ctrl_value(argv[++i]);
            else if (a.rfind("-O", 0) == 0 && a.size() > 2)
                o_dir = parse_wpa_ctrl_value(a.substr(2));
            else if (a == "-C" && i + 1 < argv.size())
                c_val = argv[++i];
            else if (a.rfind("-C", 0) == 0 && a.size() > 2)
                c_val = a.substr(2);
            else if (a == "-c" && i + 1 < argv.size())
                conf_path = argv[++i];
            else if (a.rfind("-c", 0) == 0 && a.size() > 2)
                conf_path = a.substr(2);
        }

        if (!o_dir.empty())
            return o_dir;
        if (!conf_path.empty()) {
            std::string conf_dir = helix::wifi::detail::read_ctrl_interface_from_conf(conf_path);
            if (!conf_dir.empty())
                return conf_dir;
        }
        if (!c_val.empty())
            return parse_wpa_ctrl_value(std::move(c_val));
    }
    return {};
}

/// Ordered list of directories that may hold wpa_supplicant control sockets.
///
/// Standard distros use /run/wpa_supplicant (with the legacy /var/run alias).
/// Vendor firmwares sometimes relocate the control interface — Creality's K2
/// Plus launches `wpa_supplicant ... -O /etc/wifi/wpa_supplicant/sockets`, which
/// left network discovery broken until users hand-symlinked the sockets into
/// /var/run.  We therefore (1) honour an explicit override, (2) auto-detect the
/// live -O path from the running daemon, then (3) fall back to the known-good
/// locations (including the Creality path).
static std::vector<std::string> wpa_socket_dirs() {
    std::vector<std::string> dirs;
    auto add = [&dirs](std::string d) {
        // Trim trailing slashes so de-duplication is stable.
        while (d.size() > 1 && d.back() == '/')
            d.pop_back();
        if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(std::move(d));
    };

    if (const char* env = std::getenv("HELIX_WPA_SOCKET_DIR"))
        add(env);
    add(detect_wpa_ctrl_dir_from_proc());
    add("/run/wpa_supplicant");
    add("/var/run/wpa_supplicant");
    add("/etc/wifi/wpa_supplicant/sockets"); // Creality K2 Plus / Creality OS

    return dirs;
}

WifiBackendWpaSupplicant::WifiBackendWpaSupplicant()
    : hv::EventLoopThread(nullptr), conn(nullptr),
      mon_conn(nullptr) // Initialize monitor connection
{
    spdlog::debug("[WifiBackend] Initialized (wpa_supplicant mode)");
}

WifiBackendWpaSupplicant::~WifiBackendWpaSupplicant() {
    spdlog::trace("[WifiBackend] Destructor called");

    // Signal the init thread to abort (it checks this flag between operations)
    shutdown_requested_ = true;

    // Join the async init worker (if any) so it can't call start() into us
    // while we're tearing down.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Stop the event loop and join the thread BEFORE freeing resources.
    // This prevents the use-after-free race (GitHub issue #8) where
    // cleanup_wpa() frees conn/mon_conn while init_wpa() is still using them.
    hv::EventLoopThread::stop();
    hv::EventLoopThread::join();

    // Thread is now fully stopped - safe to free resources
    cleanup_wpa();
}

WiFiError WifiBackendWpaSupplicant::start() {
    spdlog::debug("[WifiBackend] Starting wpa_supplicant backend...");
    shutdown_requested_ = false;

    // Pre-flight checks before starting event loop
    WiFiError preflight_result = check_system_prerequisites();
    if (!preflight_result.success()) {
        // User-facing critical error - wpa_supplicant not running or no permissions
        // In silent mode (e.g., HomePanel signal probe), only log - don't show modals
        if (is_silent()) {
            spdlog::debug("[WifiBackend] Pre-flight failed (silent mode): {}",
                          preflight_result.technical_msg);
        } else if (preflight_result.result == WiFiResult::SERVICE_NOT_RUNNING) {
            NOTIFY_ERROR_MODAL("WiFi Service Not Running",
                               "wpa_supplicant is not running. WiFi features unavailable.");
        } else if (preflight_result.result == WiFiResult::PERMISSION_DENIED) {
            NOTIFY_ERROR_MODAL("WiFi Permission Denied", "{}",
                               preflight_result.user_msg.empty() ? preflight_result.technical_msg
                                                                 : preflight_result.user_msg);
        } else {
            LOG_ERROR_INTERNAL("Pre-flight check failed: {}", preflight_result.technical_msg);
        }
        return preflight_result;
    }

    if (event_loop_active()) {
        // Thread already running AND a prior init actually connected — nothing to do.
        // We gate on init_succeeded_ (not init_complete_): a *failed* prior init
        // also sets init_complete_, and keying off it here would early-return
        // success and silently skip the retry, leaving the backend permanently
        // dead after a transient first-boot failure (helixscreen#1036).
        if (init_complete_.load() && init_succeeded_.load()) {
            spdlog::debug("[WifiBackend] Already running and initialized");
            return WiFiErrorHelper::success();
        }
        // Thread running but not connected (after stop(), or a prior init failed)
        // — re-initialize the wpa connections.
        spdlog::info("[WifiBackend] (Re)initializing WiFi on existing event loop");
        init_complete_ = false;
        loop()->runInLoop(std::bind(&WifiBackendWpaSupplicant::init_wpa, this));
    } else {
        // Start new event loop thread with initialization callback
        spdlog::info("[WifiBackend] Starting event loop thread");
        init_complete_ = false;
        try {
            hv::EventLoopThread::start(true, [this]() -> int {
                WifiBackendWpaSupplicant::init_wpa();
                return 0;
            });
        } catch (const std::exception& e) {
            return WiFiErrorHelper::connection_failed("Failed to start event loop: " +
                                                      std::string(e.what()));
        }
    }

    // Wait for init_wpa() to complete (with timeout)
    {
        std::unique_lock<std::mutex> lock(init_mutex_);
        if (!init_cv_.wait_for(lock, std::chrono::seconds(5),
                               [this] { return init_complete_.load(); })) {
            spdlog::error("[WifiBackend] Initialization timed out after 5 seconds");
            return WiFiError(WiFiResult::TIMEOUT, "Backend initialization timed out",
                             "WiFi system took too long to start");
        }
    }

    // init_complete_ fired, but that only means the attempt finished. If
    // init_wpa() bailed (socket not found, connect/attach failed) it dispatched
    // INIT_FAILED and left init_succeeded_ false. Report that honestly so
    // set_enabled() surfaces the failure and start_async() fires INIT_FAILED
    // (not READY) — and so the next start() retries instead of short-circuiting.
    if (!init_succeeded_.load()) {
        return WiFiErrorHelper::connection_failed(
            "wpa_supplicant init did not complete (no live control connection)");
    }

    spdlog::info("[WifiBackend] Backend initialized successfully");
    return WiFiErrorHelper::success();
}

void WifiBackendWpaSupplicant::start_async() {
    // Non-blocking variant: run start() on a worker thread. Fire READY on
    // success or INIT_FAILED on failure so callers can react without
    // blocking on socket discovery / event-loop startup.
    bool expected = false;
    if (!async_init_in_progress_.compare_exchange_strong(expected, true)) {
        spdlog::debug("[WifiBackend] wpa: start_async already in progress");
        return;
    }
    // Already connected — replay READY for late subscribers. A *failed* prior
    // init also sets init_complete_, so we additionally require init_succeeded_;
    // otherwise we'd falsely report READY and never retry (helixscreen#1036).
    if (init_complete_.load() && init_succeeded_.load()) {
        async_init_in_progress_ = false;
        dispatch_event("READY", "");
        return;
    }

    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // Wrap — EAGAIN under thread exhaustion throws std::system_error ([L083]).
    try {
        async_init_thread_ = std::thread([this]() {
            WiFiError result = start();
            bool ran_init = init_complete_.load();
            async_init_in_progress_ = false;
            if (result.success()) {
                dispatch_event("READY", "");
            } else if (!ran_init) {
                // start() failed before init_wpa() ran (preflight / thread spawn),
                // so no INIT_FAILED was dispatched there — emit it here. When
                // init_wpa() did run and fail it already dispatched INIT_FAILED;
                // re-dispatching would double-notify the manager.
                dispatch_event("INIT_FAILED", result.technical_msg);
            }
        });
    } catch (const std::system_error& e) {
        spdlog::error("[WifiBackend wpa] Failed to spawn init thread: {}", e.what());
        async_init_in_progress_ = false;
        dispatch_event("INIT_FAILED", "system busy");
    }
}

void WifiBackendWpaSupplicant::stop() {
    // Join any outstanding async init worker before teardown so it can't
    // race with cleanup_wpa() running on the event loop thread.
    if (async_init_thread_.joinable()) {
        async_init_thread_.join();
    }

    // NOTE: We intentionally do NOT stop the EventLoopThread here.
    // libhv's EventLoopThread doesn't support restart after stop(), so we keep
    // the thread running and just cleanup wpa connections. This allows set_enabled()
    // toggle to work reliably.

    if (!init_complete_.load()) {
        spdlog::trace("[WifiBackend] Already stopped (init not complete)");
        return;
    }

    spdlog::info("[WifiBackend] Disabling WiFi backend (keeping event loop alive)");

    // Reset init state so is_running() returns false and start() can re-init.
    // Clear init_succeeded_ too — the control connections are about to be torn
    // down, so the backend is no longer "up".
    init_succeeded_ = false;
    init_complete_ = false;

    // THREAD SAFETY: cleanup_wpa() manipulates libhv I/O handles (hio_read_stop,
    // hio_close) which MUST run on the event loop thread. Schedule via runInLoop()
    // with synchronization to ensure cleanup completes before stop() returns.
    if (event_loop_active() && loop()) {
        std::promise<void> cleanup_done;
        std::future<void> cleanup_future = cleanup_done.get_future();

        loop()->runInLoop([this, &cleanup_done]() {
            cleanup_wpa();
            cleanup_done.set_value();
        });

        // Wait for cleanup to complete (with timeout to prevent deadlock)
        if (cleanup_future.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
            spdlog::warn("[WifiBackend] Cleanup timed out after 2 seconds");
        }
    } else {
        // Event loop not running - cleanup directly (safe since no I/O callbacks can fire)
        cleanup_wpa();
    }

    spdlog::debug("[WifiBackend] WiFi backend disabled");
}

void WifiBackendWpaSupplicant::register_event_callback(
    const std::string& name, std::function<void(const std::string&)> callback) {
    // THREAD SAFETY: Lock callbacks map during access
    std::lock_guard<std::mutex> lock(callbacks_mutex_);

    const auto& entry = callbacks.find(name);
    if (entry == callbacks.end()) {
        callbacks.insert({name, callback});
        spdlog::debug("[WifiBackend] Registered callback '{}'", name);
    } else {
        // Callback already exists - could replace it, but parent doesn't
        LOG_WARN_INTERNAL("Callback '{}' already registered (not replacing)", name);
    }
}

// ============================================================================
// System Validation and Permission Checking
// ============================================================================

WiFiError WifiBackendWpaSupplicant::check_system_prerequisites() {
    spdlog::debug("[WifiBackend] Performing system prerequisites check");

    // 1. Check WiFi hardware availability
    WiFiError hw_result = check_wifi_hardware();
    if (!hw_result.success()) {
        return hw_result;
    }

    // 2. Check if any wpa_supplicant sockets exist
    std::vector<std::string> socket_paths = wpa_socket_dirs();
    bool socket_found = false;
    std::string accessible_socket;

    for (const auto& base_path : socket_paths) {
        if (fs::exists(base_path) && fs::is_directory(base_path)) {
            spdlog::debug("[WifiBackend] Found wpa_supplicant directory: {}", base_path);

            // Look for interface sockets (use error_code overload to handle permission denied)
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(base_path, ec)) {
                if (ec) {
                    spdlog::debug("[WifiBackend] Cannot iterate {}: {}", base_path, ec.message());
                    break;
                }
                if (fs::is_socket(entry.path())) {
                    std::string socket_path = entry.path().string();

                    // Skip P2P sockets
                    if (socket_path.find("p2p") == std::string::npos) {
                        socket_found = true;
                        spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", socket_path);

                        // Check permissions for this socket
                        WiFiError perm_result = check_socket_permissions(socket_path);
                        if (perm_result.success()) {
                            accessible_socket = socket_path;
                            break;
                        } else {
                            LOG_WARN_INTERNAL("Socket {} permission check failed: {}", socket_path,
                                              perm_result.technical_msg);
                        }
                    }
                }
            }
            if (ec && !socket_found) {
                spdlog::debug("[WifiBackend] Permission denied iterating {}", base_path);
            }
            if (!accessible_socket.empty())
                break;
        }
    }

    if (!socket_found) {
        return WiFiErrorHelper::service_not_running("wpa_supplicant (no control sockets found)");
    }

    if (accessible_socket.empty()) {
        return WiFiErrorHelper::permission_denied("Found wpa_supplicant sockets but cannot access "
                                                  "them - check user permissions (netdev group)");
    }

    spdlog::debug("[WifiBackend] System prerequisites check passed - accessible socket: {}",
                  accessible_socket);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_socket_permissions(const std::string& socket_path) {
    spdlog::trace("[WifiBackend] Checking permissions for socket: {}", socket_path);

    // Try to open a test connection (use wpa_ctrl_open2 to avoid PrivateTmp issues)
    const char* cli_dir = resolve_wpa_client_dir();
    struct wpa_ctrl* test_ctrl = wpa_ctrl_open2(socket_path.c_str(), cli_dir);
    if (!test_ctrl) {
        // Get more specific error information
        int err = errno;
        std::string error_detail = "wpa_ctrl_open failed: " + std::string(strerror(err));

        if (err == EACCES || err == EPERM) {
            return WiFiErrorHelper::permission_denied(error_detail +
                                                      " (try adding user to netdev group)");
        } else if (err == ENOENT) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant socket not found");
        } else if (err == ECONNREFUSED) {
            return WiFiErrorHelper::service_not_running("wpa_supplicant daemon not responding");
        } else {
            return WiFiErrorHelper::connection_failed(error_detail);
        }
    }

    // Test connection successful - close it immediately
    wpa_ctrl_close(test_ctrl);
    spdlog::debug("[WifiBackend] Socket permission check passed: {}", socket_path);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::check_wifi_hardware() {
    spdlog::trace("[WifiBackend] Checking WiFi hardware availability");

    // Check for common WiFi interface patterns in /sys/class/net
    bool wifi_found = false;
    std::string interface_name;

    try {
        const std::string net_path = "/sys/class/net";
        if (fs::exists(net_path)) {
            for (const auto& entry : fs::directory_iterator(net_path)) {
                std::string iface = entry.path().filename().string();

                // Check for common WiFi interface patterns
                if (iface.find("wlan") == 0 || iface.find("wlp") == 0 || iface.find("wlx") == 0 ||
                    iface.find("wifi") == 0) {
                    // Verify it's a wireless interface by checking for wireless directory
                    std::string wireless_path = entry.path().string() + "/wireless";
                    if (fs::exists(wireless_path)) {
                        wifi_found = true;
                        interface_name = iface;
                        spdlog::debug("[WifiBackend] Found WiFi interface: {}", iface);
                        break;
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking WiFi interfaces: {}", e.what());
        // Don't fail entirely - this might be a permission issue or unusual system
    }

    if (!wifi_found) {
        return WiFiErrorHelper::hardware_not_available();
    }

    // Check RF-kill status
    try {
        const std::string rfkill_path = "/sys/class/rfkill";
        if (fs::exists(rfkill_path)) {
            for (const auto& entry : fs::directory_iterator(rfkill_path)) {
                std::string type_file = entry.path().string() + "/type";
                if (fs::exists(type_file)) {
                    std::ifstream type_stream(type_file);
                    std::string type;
                    if (type_stream >> type && type == "wlan") {
                        // Check if soft-blocked
                        std::string soft_file = entry.path().string() + "/soft";
                        if (fs::exists(soft_file)) {
                            std::ifstream soft_stream(soft_file);
                            int soft_blocked;
                            if (soft_stream >> soft_blocked && soft_blocked == 1) {
                                return WiFiErrorHelper::rf_kill_blocked();
                            }
                        }
                        break;
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOG_WARN_INTERNAL("Error checking RF-kill status: {}", e.what());
        // Continue - RF-kill check is nice-to-have
    }

    spdlog::debug("[WifiBackend] WiFi hardware check passed - interface: {}", interface_name);
    return WiFiErrorHelper::success();
}

// ============================================================================
// wpa_supplicant Communication
// ============================================================================

void WifiBackendWpaSupplicant::init_wpa() {
    spdlog::trace("[WifiBackend] init_wpa() called in event loop thread");

    // Clear the success flag up front; only the fully-connected tail below sets
    // it true. Every early-return failure path therefore leaves it false.
    init_succeeded_ = false;

    // Socket discovery: Try common paths
    std::string wpa_socket;
    bool socket_found = false;

    // Try common wpa_supplicant socket paths (plus override / auto-detected / vendor)
    // Use error_code overload to handle permission denied gracefully (non-root users)
    std::vector<std::string> socket_dirs = wpa_socket_dirs();
    for (const auto& base_path : socket_dirs) {
        if (socket_found)
            break;
        if (!fs::exists(base_path) || !fs::is_directory(base_path))
            continue;

        spdlog::debug("[WifiBackend] Searching for wpa_supplicant socket in {}", base_path);

        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(base_path, ec)) {
            if (ec) {
                spdlog::debug("[WifiBackend] Cannot iterate {}: {}", base_path, ec.message());
                break;
            }
            if (fs::is_socket(entry.path())) {
                std::string socket_path = entry.path().string();

                // Filter out P2P sockets (e.g., p2p-dev-wlan0)
                if (socket_path.find("p2p") == std::string::npos) {
                    wpa_socket = socket_path;
                    socket_found = true;
                    spdlog::debug("[WifiBackend] Found wpa_supplicant socket: {}", wpa_socket);
                    break;
                }
            }
        }
    }

    // Helper to signal completion and notify waiters
    auto signal_init_complete = [this]() {
        init_complete_ = true;
        init_cv_.notify_all();
    };

    if (!socket_found) {
        LOG_ERROR_INTERNAL(
            "Could not find wpa_supplicant socket in any known location "
            "(/run, /var/run, /etc/wifi/wpa_supplicant/sockets, $HELIX_WPA_SOCKET_DIR)");
        LOG_ERROR_INTERNAL("Is wpa_supplicant daemon running? Override its control dir "
                           "with HELIX_WPA_SOCKET_DIR if it uses a non-standard path.");
        dispatch_event("INIT_FAILED", "wpa_supplicant socket not found");
        signal_init_complete();
        return;
    }

    // Check for shutdown before opening connections
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested during init, aborting");
        signal_init_complete();
        return;
    }

    // Use wpa_ctrl_open2() so the client reply socket is created outside
    // systemd's PrivateTmp namespace (wpa_supplicant must be able to sendto()
    // the reply address).  Falls back to default /tmp for non-systemd runs.
    const char* cli_dir = resolve_wpa_client_dir();
    if (cli_dir) {
        spdlog::debug("[WifiBackend] Using client socket dir: {}", cli_dir);
    }

    // Open control connection (for sending commands)
    if (conn == nullptr) {
        conn = wpa_ctrl_open2(wpa_socket.c_str(), cli_dir);
        if (conn == nullptr) {
            LOG_ERROR_INTERNAL("Failed to open control connection to {}", wpa_socket);
            dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant");
            signal_init_complete();
            return;
        }
        spdlog::debug("[WifiBackend] Opened control connection");
    }

    // Check for shutdown before opening monitor connection
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested during init, aborting");
        signal_init_complete();
        return;
    }

    // Open monitor connection (for receiving events)
    mon_conn = wpa_ctrl_open2(wpa_socket.c_str(),
                              cli_dir); // SECURITY: Use member variable to prevent leak
    if (mon_conn == nullptr) {
        LOG_ERROR_INTERNAL("Failed to open monitor connection to {}", wpa_socket);
        dispatch_event("INIT_FAILED", "Failed to connect to wpa_supplicant monitor");
        signal_init_complete();
        return;
    }

    // Check for shutdown before the potentially blocking wpa_ctrl_attach().
    // This is the critical check: wpa_ctrl_attach() can block for 5+ seconds
    // when wpa_supplicant is unresponsive, and the destructor may be waiting.
    if (shutdown_requested_.load()) {
        spdlog::debug("[WifiBackend] Shutdown requested before attach, aborting");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr;
        signal_init_complete();
        return;
    }

    // Attach to wpa_supplicant event stream
    if (wpa_ctrl_attach(mon_conn) != 0) {
        LOG_ERROR_INTERNAL("Failed to attach to wpa_supplicant events");
        dispatch_event("INIT_FAILED", "Failed to attach to wpa_supplicant events");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr; // Clear member to avoid double-close
        signal_init_complete();
        return;
    }
    spdlog::debug("[WifiBackend] Attached to wpa_supplicant event stream");

    // Get file descriptor for monitor socket
    int monfd = wpa_ctrl_get_fd(mon_conn);
    if (monfd < 0) {
        LOG_ERROR_INTERNAL("Failed to get monitor socket file descriptor");
        dispatch_event("INIT_FAILED", "Failed to initialize wpa_supplicant communication");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr; // Clear member to avoid double-close
        signal_init_complete();
        return;
    }
    spdlog::trace("[WifiBackend] Monitor socket fd: {}", monfd);

    // Register with libhv event loop for async I/O
    mon_io_ = hio_get(loop()->loop(), monfd);
    if (mon_io_ == nullptr) {
        LOG_ERROR_INTERNAL("Failed to register monitor socket with libhv");
        dispatch_event("INIT_FAILED", "Failed to initialize WiFi event handling");
        wpa_ctrl_close(mon_conn);
        mon_conn = nullptr;
        signal_init_complete();
        return;
    }

    // Set up I/O callbacks
    hio_set_context(mon_io_, this); // Store 'this' pointer for static callback
    hio_setcb_read(mon_io_, WifiBackendWpaSupplicant::_handle_wpa_events); // Static trampoline
    hio_read_start(mon_io_); // Start monitoring socket for events

    resolve_5ghz_support();

    // Reached only with live control + monitor connections registered — the
    // backend is genuinely up. This is the one place init_succeeded_ goes true.
    init_succeeded_ = true;

    spdlog::debug("[WifiBackend] wpa_supplicant backend initialized successfully");
    signal_init_complete();
}

void WifiBackendWpaSupplicant::cleanup_wpa() {
    spdlog::trace("[WifiBackend] Cleaning up wpa_supplicant connections");

    // Stop libhv I/O monitoring BEFORE closing the socket
    // This prevents callbacks on a closed fd and allows re-registration
    if (mon_io_) {
        spdlog::trace("[WifiBackend] Stopping libhv I/O monitoring");
        hio_read_stop(mon_io_);
        hio_close(mon_io_);
        mon_io_ = nullptr;
    }

    // Close monitor connection (detach from events)
    if (mon_conn) {
        spdlog::trace("[WifiBackend] Detaching from wpa_supplicant events");
        wpa_ctrl_detach(mon_conn); // Detach from event stream
        wpa_ctrl_close(mon_conn);  // Close monitor connection
        mon_conn = nullptr;
    }

    // Close control connection (under lock to avoid closing while send_command is using it)
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        if (conn) {
            spdlog::trace("[WifiBackend] Closing wpa_supplicant control connection");
            wpa_ctrl_close(conn);
            conn = nullptr;
        }
    }

    spdlog::debug("[WifiBackend] wpa_supplicant connections cleaned up");
}

std::string WifiBackendWpaSupplicant::map_event_to_callback(const std::string& event) {
    // Map wpa_supplicant events to registered callback names
    // Only actionable events are mapped; informational events return empty string

    if (event.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
        return "SCAN_COMPLETE";
    }
    if (event.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
        return "CONNECTED";
    }
    if (event.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
        return "DISCONNECTED";
    }
    // Auth failures can come in multiple forms
    if (event.find("CTRL-EVENT-SSID-TEMP-DISABLED") != std::string::npos &&
        event.find("WRONG_KEY") != std::string::npos) {
        return "AUTH_FAILED";
    }
    if (event.find("CTRL-EVENT-AUTH-REJECT") != std::string::npos) {
        return "AUTH_FAILED";
    }

    // Informational events - no callback needed
    // SCAN-STARTED, BSS-ADDED, BSS-REMOVED, REGDOM-CHANGE, SIGNAL-CHANGE, etc.
    return "";
}

void WifiBackendWpaSupplicant::handle_wpa_events(void* data, int len) {
    if (data == nullptr || len <= 0) {
        LOG_WARN_INTERNAL("Received empty event");
        return;
    }

    // Convert to string (may contain newlines)
    std::string event = std::string(static_cast<char*>(data), len);

    spdlog::trace("[WifiBackend] Event received: {}", event);

    // Determine which callback (if any) should receive this event
    std::string callback_name = map_event_to_callback(event);

    if (callback_name.empty()) {
        // Informational event - no callback needed
        spdlog::trace("[WifiBackend] Ignoring informational event (no matching callback)");
        return;
    }

    // THREAD SAFETY: Copy callback out under the mutex, then release BEFORE
    // invoking. Holding callbacks_mutex_ across the callback invites deadlock
    // if a handler acquires another backend lock or re-enters the backend.
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks.find(callback_name);
        if (it == callbacks.end()) {
            spdlog::trace("[WifiBackend] No callback registered for event type: {}", callback_name);
            return;
        }
        cb = it->second;
    }

    spdlog::debug("[WifiBackend] Dispatching {} event to callback", callback_name);
    try {
        cb(event);
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("Exception in callback '{}': {}", callback_name, e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", callback_name);
    }
}

void WifiBackendWpaSupplicant::_handle_wpa_events(hio_t* io, void* data, int readbyte) {
    // Static trampoline: Extract instance pointer and forward to member function
    WifiBackendWpaSupplicant* instance = static_cast<WifiBackendWpaSupplicant*>(hio_context(io));
    if (instance) {
        instance->handle_wpa_events(data, readbyte);
    } else {
        LOG_ERROR_INTERNAL("Static callback invoked with NULL context");
    }
}

void WifiBackendWpaSupplicant::dispatch_event(const std::string& event_name,
                                              const std::string& message) {
    // Dispatch to a specific registered callback (for synthetic events like INIT_FAILED).
    // Copy the callback out under the mutex, then release BEFORE invoking — same
    // deadlock-avoidance rationale as handle_wpa_events().
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks.find(event_name);
        if (it == callbacks.end()) {
            return;
        }
        cb = it->second;
    }

    spdlog::debug("[WifiBackend] Dispatching synthetic event '{}': {}", event_name, message);
    try {
        cb(message);
    } catch (const std::exception& e) {
        LOG_ERROR_INTERNAL("Exception in callback '{}': {}", event_name, e.what());
    } catch (...) {
        LOG_ERROR_INTERNAL("Unknown exception in callback '{}'", event_name);
    }
}

// Helper function to sanitize commands for logging (remove passwords)
static std::string sanitize_command_for_log(const std::string& cmd) {
    // Check if command contains password
    if (cmd.find(" psk ") != std::string::npos) {
        size_t psk_pos = cmd.find(" psk ");
        return cmd.substr(0, psk_pos + 5) + "\"[REDACTED]\"";
    }
    return cmd;
}

std::string WifiBackendWpaSupplicant::send_command(const std::string& cmd) {
    // Lock to prevent concurrent access from LVGL thread (get_status) and
    // event loop thread (get_scan_results) — wpa_ctrl is not thread-safe.
    std::lock_guard<std::mutex> lock(cmd_mutex_);

    if (conn == nullptr) {
        // Normal during the brief window between start_async() dispatch and
        // init_wpa() opening the control connection — UI callers (e.g. the
        // home-panel network widget) race the worker thread on boot. Keep at
        // debug so it's available when troubleshooting but doesn't surface as
        // a user-visible warning in the shipping build.
        spdlog::debug("[WifiBackend] send_command called but not connected to wpa_supplicant");
        return "";
    }

    char resp[4096];
    size_t len = sizeof(resp) - 1;

    // SECURITY: Don't log passwords
    std::string safe_cmd = sanitize_command_for_log(cmd);
    spdlog::trace("[WifiBackend] Sending command: {}", safe_cmd);

    int result = wpa_ctrl_request(conn, cmd.c_str(), cmd.length(), resp, &len, nullptr);
    if (result != 0) {
        LOG_ERROR_INTERNAL("Command failed: {} (error code: {})", safe_cmd, result);
        return "";
    }

    // SECURITY: Validate len before using as array index
    if (len >= sizeof(resp)) {
        LOG_ERROR_INTERNAL("Response too large: {} bytes", len);
        return "";
    }

    // Null-terminate response
    resp[len] = '\0';

    // SECURITY: Don't log password responses
    if (cmd.find(" psk ") == std::string::npos) {
        spdlog::trace("[WifiBackend] Command response ({} bytes): {}", len, std::string(resp, len));
    } else {
        spdlog::trace("[WifiBackend] Command response ({} bytes): [REDACTED]", len);
    }

    return std::string(resp, len);
}

// ============================================================================
// WifiBackend Interface Implementation
// ============================================================================

bool WifiBackendWpaSupplicant::is_running() const {
    // "Running" means we have live wpa_supplicant connections, not merely that
    // an init attempt finished. Keying off init_succeeded_ (not init_complete_)
    // means a failed init reports as not-running, so trigger_scan()/is_enabled()
    // surface "not ready" honestly instead of a contradictory "up but every
    // command fails" state (helixscreen#1036). The event-loop thread may still
    // be alive after stop(); that's fine — init_succeeded_ is false then too.
    return init_succeeded_.load();
}

WiFiError WifiBackendWpaSupplicant::trigger_scan() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string result = send_command("SCAN");
    if (result == "OK\n") {
        spdlog::debug("[WifiBackend] Scan triggered successfully");
        return WiFiErrorHelper::success();
    } else if (result.empty()) {
        return WiFiErrorHelper::connection_failed("No response from wpa_supplicant SCAN command");
    } else if (result.find("FAIL") != std::string::npos) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN command failed: " + result,
                         "Failed to start network scan", "Check WiFi interface status");
    } else {
        spdlog::warn("[WifiBackend] Unexpected scan response: {}", result);
        return WiFiError(WiFiResult::BACKEND_ERROR, "Unexpected scan response: " + result,
                         "Network scan returned unexpected result");
    }
}

// Deduplicate networks by SSID, keeping the strongest signal for each.
// In mesh WiFi systems, multiple APs broadcast the same SSID - show only the best one.
static std::vector<WiFiNetwork> deduplicate_by_ssid(std::vector<WiFiNetwork>& networks) {
    std::unordered_map<std::string, size_t> best_index_by_ssid;

    for (size_t i = 0; i < networks.size(); ++i) {
        const auto& net = networks[i];
        auto it = best_index_by_ssid.find(net.ssid);
        if (it == best_index_by_ssid.end()) {
            best_index_by_ssid[net.ssid] = i;
        } else if (net.signal_strength > networks[it->second].signal_strength) {
            it->second = i;
        }
    }

    std::vector<WiFiNetwork> result;
    result.reserve(best_index_by_ssid.size());
    for (const auto& [ssid, idx] : best_index_by_ssid) {
        result.push_back(networks[idx]);
    }

    if (result.size() < networks.size()) {
        spdlog::debug("[WifiBackend] Deduplicated {} networks to {} unique SSIDs", networks.size(),
                      result.size());
    }

    return result;
}

WiFiError WifiBackendWpaSupplicant::get_scan_results(std::vector<WiFiNetwork>& networks) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string raw = send_command("SCAN_RESULTS");
    if (raw.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant SCAN_RESULTS command");
    }

    if (raw.find("FAIL") != std::string::npos) {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant SCAN_RESULTS failed: " + raw,
                         "Failed to retrieve scan results");
    }

    try {
        networks = parse_scan_results(raw);
        networks = deduplicate_by_ssid(networks);
        spdlog::debug("[WifiBackend] Retrieved {} unique networks", networks.size());
        return WiFiErrorHelper::success();
    } catch (const std::exception& e) {
        return WiFiError(WiFiResult::BACKEND_ERROR,
                         "Failed to parse scan results: " + std::string(e.what()),
                         "Error processing network scan data");
    }
}

// Helper function to validate and escape wpa_supplicant strings
static std::string validate_wpa_string(const std::string& input, const std::string& field_name) {
    // Check for dangerous characters that could enable command injection
    for (char c : input) {
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c < 32 || c == 127) {
            LOG_ERROR_INTERNAL("Invalid character in {}: ASCII {}", field_name, (int)c);
            return "";
        }
    }

    // Additional checks for reasonable limits
    if (input.empty() || input.length() > 255) {
        LOG_ERROR_INTERNAL("Invalid {} length: {}", field_name, input.length());
        return "";
    }

    return input;
}

WiFiError WifiBackendWpaSupplicant::connect_network(const std::string& ssid,
                                                    const std::string& password) {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    // SECURITY: Validate inputs to prevent command injection
    std::string clean_ssid = validate_wpa_string(ssid, "SSID");
    if (clean_ssid.empty()) {
        return WiFiError(WiFiResult::INVALID_PARAMETERS,
                         "SSID contains invalid characters (quotes, control chars, etc.)",
                         "Invalid network name", "Check that the network name is correct");
    }

    std::string clean_password = validate_wpa_string(password, "password");
    if (!password.empty() && clean_password.empty()) {
        return WiFiErrorHelper::authentication_failed(ssid +
                                                      " (password contains invalid characters)");
    }

    spdlog::info("[WifiBackend] Connecting to network '{}'", clean_ssid);

    // Step 1: Add new network (get network ID)
    std::string add_result = send_command("ADD_NETWORK");
    if (add_result.empty() || add_result == "FAIL\n") {
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to add network to wpa_supplicant");
    }

    // Parse network ID (should be a number)
    std::string network_id = add_result;
    // Remove trailing newline
    if (!network_id.empty() && network_id.back() == '\n') {
        network_id.pop_back();
    }

    // SECURITY: Validate network ID is actually a number
    for (char c : network_id) {
        if (!std::isdigit(c)) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             "wpa_supplicant returned invalid network ID: " + network_id,
                             "Internal WiFi error", "Try restarting WiFi services");
        }
    }

    spdlog::debug("[WifiBackend] Added network with ID: {}", network_id);

    // Step 2: Set SSID
    std::string set_ssid_cmd = "SET_NETWORK " + network_id + " ssid \"" + clean_ssid + "\"";
    std::string ssid_result = send_command(set_ssid_cmd);
    if (ssid_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to set SSID: {}", ssid_result);
        // Clean up: remove the network
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to configure network SSID");
    }

    // Step 3: Set security (PSK for secured networks, key_mgmt for open)
    if (clean_password.empty()) {
        // Open network
        std::string set_open_cmd = "SET_NETWORK " + network_id + " key_mgmt NONE";
        std::string open_result = send_command(set_open_cmd);
        if (open_result != "OK\n") {
            LOG_ERROR_INTERNAL("Failed to set open security: {}", open_result);
            send_command("REMOVE_NETWORK " + network_id);
            NOTIFY_ERROR("Failed to save WiFi network");
            return WiFiErrorHelper::connection_failed("Failed to configure open network security");
        }
        spdlog::debug("[WifiBackend] Configured as open network");
    } else {
        // Secured network with PSK
        std::string set_psk_cmd = "SET_NETWORK " + network_id + " psk \"" + clean_password + "\"";
        std::string psk_result = send_command(set_psk_cmd);
        if (psk_result != "OK\n") {
            LOG_ERROR_INTERNAL(
                "Failed to set PSK"); // Don't log the actual result (may contain password)
            send_command("REMOVE_NETWORK " + network_id);
            NOTIFY_ERROR("Failed to connect to '{}'. Check password.", clean_ssid);
            return WiFiErrorHelper::authentication_failed(ssid);
        }
        spdlog::debug("[WifiBackend] Configured with PSK");
    }

    // Step 4: Enable and select network
    std::string enable_cmd = "ENABLE_NETWORK " + network_id;
    std::string enable_result = send_command(enable_cmd);
    if (enable_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to enable network: {}", enable_result);
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to save WiFi network");
        return WiFiErrorHelper::connection_failed("Failed to enable network configuration");
    }
    spdlog::debug("[WifiBackend] Network {} enabled, selecting for connection", network_id);

    // Step 5: Select network (disconnect others)
    std::string select_cmd = "SELECT_NETWORK " + network_id;
    std::string select_result = send_command(select_cmd);
    if (select_result != "OK\n") {
        LOG_ERROR_INTERNAL("Failed to select network: {}", select_result);
        send_command("REMOVE_NETWORK " + network_id);
        NOTIFY_ERROR("Failed to connect to '{}'", clean_ssid);
        return WiFiErrorHelper::connection_failed("Failed to select network for connection");
    }

    // Step 6: Save config so WiFi persists across reboots
    // wpa_supplicant writes to its -c config file (which may be symlinked to
    // persistent storage by platform-specific scripts, e.g. Snapmaker U1's
    // /etc/network/if-pre-up.d/wpa-conf.sh)
    std::string save_result = send_command("SAVE_CONFIG");
    if (save_result == "OK\n") {
        spdlog::debug("[WifiBackend] Configuration saved to disk");
    } else {
        spdlog::warn("[WifiBackend] SAVE_CONFIG failed (non-fatal): {}", save_result);
    }

    spdlog::info("[WifiBackend] Network configuration complete, connecting to '{}'", ssid);
    return WiFiErrorHelper::success();
}

WiFiError WifiBackendWpaSupplicant::disconnect_network() {
    if (!is_running()) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                         "WiFi system not ready");
    }

    std::string result = send_command("DISCONNECT");
    if (result == "OK\n") {
        spdlog::debug("[WifiBackend] Disconnect successful");
        return WiFiErrorHelper::success();
    } else if (result.empty()) {
        return WiFiErrorHelper::connection_failed(
            "No response from wpa_supplicant DISCONNECT command");
    } else {
        return WiFiError(WiFiResult::BACKEND_ERROR, "wpa_supplicant DISCONNECT failed: " + result,
                         "Failed to disconnect from network");
    }
}

WifiBackend::ConnectionStatus WifiBackendWpaSupplicant::get_status() {
    ConnectionStatus status = {};
    status.connected = false;

    std::string raw_status = send_command("STATUS");
    if (raw_status.empty()) {
        // send_command already logs (at debug) when conn is null during the
        // startup race; post-init empty responses propagate upward as
        // status.connected=false which is the correct observable behavior.
        spdlog::debug("[WifiBackend] Empty STATUS response");
        return status;
    }

    // Parse key=value pairs from STATUS output
    std::istringstream stream(raw_status);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty())
            continue;

        // Find key=value separator
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos)
            continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        if (key == "wpa_state") {
            // Check if fully connected
            status.connected = (value == "COMPLETED");
        } else if (key == "ssid") {
            status.ssid = value;
        } else if (key == "bssid") {
            status.bssid = value;
        } else if (key == "address") {
            status.mac_address = value;
        } else if (key == "ip_address") {
            status.ip_address = value;
        } else if (key == "freq") {
            try {
                status.frequency_mhz = std::stoi(value);
            } catch (const std::exception&) {
                // Ignore parse errors
            }
        }
    }

    // If connected, get additional signal info via SIGNAL_POLL
    if (status.connected) {
        std::string signal_raw = send_command("SIGNAL_POLL");
        if (!signal_raw.empty()) {
            std::istringstream signal_stream(signal_raw);
            std::string signal_line;

            while (std::getline(signal_stream, signal_line)) {
                size_t eq_pos = signal_line.find('=');
                if (eq_pos == std::string::npos)
                    continue;

                std::string key = signal_line.substr(0, eq_pos);
                std::string value = signal_line.substr(eq_pos + 1);

                if (key == "RSSI") {
                    try {
                        int rssi_dbm = std::stoi(value);
                        status.signal_strength = dbm_to_percentage(rssi_dbm);
                    } catch (const std::exception& e) {
                        spdlog::trace("[WifiBackend] Invalid RSSI value '{}': {}", value, e.what());
                    }
                    break; // Found what we need
                }
            }
        }
    }

    // Only log when status actually changes (reduces log noise)
    bool status_changed =
        (status.connected != last_logged_status_.connected ||
         status.ssid != last_logged_status_.ssid ||
         status.ip_address != last_logged_status_.ip_address ||
         std::abs(status.signal_strength - last_logged_status_.signal_strength) > 5);

    if (status_changed) {
        spdlog::debug("[WifiBackend] Status: connected={} ssid='{}' ip='{}' signal={}%",
                      status.connected, status.ssid, status.ip_address, status.signal_strength);
        last_logged_status_ = status;
    }

    return status;
}

bool WifiBackendWpaSupplicant::supports_5ghz() const {
    return supports_5ghz_cached_;
}

void WifiBackendWpaSupplicant::resolve_5ghz_support() {
    try {
        if (supports_5ghz_resolved_)
            return;

        // Try wpa_supplicant GET_CAPABILITY freq
        std::string freq_resp = send_command("GET_CAPABILITY freq");
        if (wifi_parse_freq_list_has_5ghz(freq_resp)) {
            supports_5ghz_cached_ = true;
            supports_5ghz_resolved_ = true;
            spdlog::debug("[WifiBackend] 5GHz support detected via GET_CAPABILITY freq");
            return;
        }

        // Fallback: try iw phy info
        // Find phy name from /sys/class/net/wlan0/phy80211/name (or similar)
        FILE* pipe = popen("iw phy phy0 info 2>/dev/null", "r");
        if (pipe) {
            std::string iw_output;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) {
                iw_output += buf;
            }
            pclose(pipe);

            if (wifi_parse_iw_phy_has_5ghz(iw_output)) {
                supports_5ghz_cached_ = true;
                supports_5ghz_resolved_ = true;
                spdlog::debug("[WifiBackend] 5GHz support detected via iw phy info");
                return;
            }
        }

        supports_5ghz_resolved_ = true;
        spdlog::debug("[WifiBackend] No 5GHz support detected (2.4GHz only)");
    } catch (const std::exception& e) {
        spdlog::warn("[WifiBackend] Error detecting 5GHz support: {}", e.what());
        supports_5ghz_resolved_ = true;
        // Keep cached = false (safe default)
    }
}

// ============================================================================
// Helper Methods (encapsulate wpa_supplicant ugliness)
// ============================================================================

std::vector<WiFiNetwork> WifiBackendWpaSupplicant::parse_scan_results(const std::string& raw) {
    std::vector<WiFiNetwork> networks;

    if (raw.empty()) {
        spdlog::debug("[WifiBackend] Empty scan results");
        return networks;
    }

    // Skip header line (bssid / frequency / signal level / flags / ssid)
    std::istringstream stream(raw);
    std::string line;
    bool skip_header = true;

    while (std::getline(stream, line)) {
        if (skip_header) {
            skip_header = false;
            continue; // Skip "bssid / frequency / signal level / flags / ssid"
        }

        if (line.empty())
            continue;

        // Parse tab-separated fields: BSSID\tfreq\tsignal\tflags\tSSID
        // Note: Hidden networks may have only 4 fields (SSID completely absent)
        std::vector<std::string> fields = split_by_tabs(line);
        if (fields.size() < 4) {
            // Truly malformed - need at least BSSID, freq, signal, flags
            spdlog::trace("[WifiBackend] Skipping malformed scan line ({} fields): {}",
                          fields.size(), line);
            continue;
        }

        std::string bssid = fields[0];
        std::string freq_str = fields[1];
        std::string signal_str = fields[2];
        std::string flags = fields[3];
        // SSID may be missing entirely for hidden networks
        std::string ssid = (fields.size() >= 5) ? fields[4] : "";

        // Skip hidden networks (empty or missing SSID)
        if (ssid.empty()) {
            spdlog::trace("[WifiBackend] Skipping hidden network: {}", bssid);
            continue;
        }

        // Parse signal strength (dBm)
        int signal_dbm = 0;
        try {
            signal_dbm = std::stoi(signal_str);
        } catch (const std::exception& e) {
            LOG_WARN_INTERNAL("Invalid signal strength '{}': {}", signal_str, e.what());
            continue;
        }

        // Convert dBm to percentage
        int signal_percent = dbm_to_percentage(signal_dbm);

        // Detect security type
        bool is_secured = false;
        std::string security_type = detect_security_type(flags, is_secured);

        // Parse frequency
        int freq_mhz = 0;
        try {
            freq_mhz = std::stoi(freq_str);
        } catch (const std::exception&) {
            // Ignore parse errors, keep 0
        }

        // Create network entry
        WiFiNetwork network(ssid, signal_percent, is_secured, security_type, freq_mhz);
        networks.push_back(network);

        spdlog::trace("[WifiBackend] Parsed network: '{}' {}% {} {}", ssid, signal_percent,
                      security_type, bssid);
    }

    spdlog::debug("[WifiBackend] Parsed {} networks from scan results", networks.size());
    return networks;
}

std::vector<std::string> WifiBackendWpaSupplicant::split_by_tabs(const std::string& str) {
    std::vector<std::string> parts;
    std::stringstream ss(str);
    std::string part;
    while (std::getline(ss, part, '\t')) {
        parts.push_back(part);
    }
    return parts;
}

int WifiBackendWpaSupplicant::dbm_to_percentage(int dbm) {
    // -30 dBm = 100% (excellent), -90 dBm = 0% (unusable)
    return std::max(0, std::min(100, (dbm + 90) * 100 / 60));
}

std::string WifiBackendWpaSupplicant::detect_security_type(const std::string& flags,
                                                           bool& is_secured) {
    if (flags.find("WPA3") != std::string::npos) {
        is_secured = true;
        return "WPA3";
    }
    if (flags.find("WPA2") != std::string::npos) {
        is_secured = true;
        return "WPA2";
    }
    if (flags.find("WPA") != std::string::npos) {
        is_secured = true;
        return "WPA";
    }
    if (flags.find("WEP") != std::string::npos) {
        is_secured = true;
        return "WEP";
    }
    is_secured = false;
    return "Open";
}

#else
// ============================================================================
// macOS Stub Implementation: No-op for simulator
// ============================================================================

// Empty file - all methods are inline in header

#endif // !__APPLE__ && !__ANDROID__

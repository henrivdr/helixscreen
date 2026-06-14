// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

struct BundleOptions {
    bool include_klipper_logs = false;
    bool include_moonraker_logs = false;
    std::string user_note;
};

struct BundleResult {
    bool success = false;
    std::string share_code;
    std::string error_message;
};

class DebugBundleCollector {
  public:
    /// Collect all debug data into JSON
    static nlohmann::json collect(const BundleOptions& options = {});

    /// Collect, compress, and upload asynchronously.
    /// Callback is invoked on the UI thread via helix::ui::queue_update().
    using ResultCallback = std::function<void(const BundleResult&)>;
    static void upload_async(const BundleOptions& options, ResultCallback callback);

    /// Individual collectors (public for testing)
    static nlohmann::json collect_system_info();
    static nlohmann::json collect_printer_info();
    static std::string collect_log_tail(int num_lines = 2000);

    /// Metadata about the log pipeline so a bundle reader knows whether debug
    /// was being captured: { target, level, ring_lines, log_tail_source }.
    static nlohmann::json collect_log_meta();
    static std::string collect_crash_txt();
    static nlohmann::json collect_sanitized_settings();
    static std::string collect_klipper_log_tail(int num_lines = 2000);
    static std::string collect_moonraker_log_tail(int num_lines = 2000);

    /// Read crash_report.txt from config_dir (persists after crash.txt consumed)
    static std::string collect_crash_report_txt(const std::string& config_dir);

    /// Read raw crash.txt from config_dir (active crash, before next-boot rotation)
    static std::string collect_crash_txt(const std::string& config_dir);

    /// Read crash_history.json from config_dir (past crash submissions)
    static nlohmann::json collect_crash_history(const std::string& config_dir);

    /// Get double-hashed device ID from telemetry_device.json (for R2 cross-ref)
    static std::string collect_device_id(const std::string& config_dir);

    /// Read log tail from an explicit ordered list of paths (testable)
    static std::string collect_log_tail_from_paths(const std::vector<std::string>& paths,
                                                   int num_lines);

    /// Collect Moonraker state via REST (server info, printer state, config)
    static nlohmann::json collect_moonraker_info();

    /// Collect filament system data (AFC, Happy Hare, ACE, Spoolman, tool changers)
    static nlohmann::json collect_filament_system_info();

    /// Filter a Klipper object list to filament-related objects (public for testing)
    static nlohmann::json filter_filament_objects(const nlohmann::json& object_list);

    /// Collect platform-specific diagnostic files (e.g., AD5X Adventurer5M.json)
    /// served via Moonraker's /server/files/<root>/<path> endpoint. Files that
    /// don't exist (404) are skipped silently; other errors are recorded in-line.
    static nlohmann::json collect_platform_files();

    /// Sanitize a string value for PII patterns (emails, credentials, webhooks, tokens, MACs)
    static std::string sanitize_value(const std::string& value);

    /// Recursively strip sensitive keys from JSON (public for integration testing)
    static nlohmann::json sanitize_json(const nlohmann::json& input, int depth = 0);

    /// Gzip compression using zlib
    static std::vector<uint8_t> gzip_compress(const std::string& data);

  private:
    static constexpr const char* WORKER_URL = "https://crash.helixscreen.org/v1/debug-bundle";
    static constexpr const char* INGEST_API_KEY = "hx-tel-v1-a7f3c9e2d1b84056";

    /// Blocking HTTP GET to a Moonraker endpoint, returns parsed JSON or error object
    static nlohmann::json moonraker_get(const std::string& base_url, const std::string& endpoint,
                                        int timeout_sec = 10);

    /// Get the Moonraker HTTP base URL (from MoonrakerAPI if connected)
    static std::string get_moonraker_url();

    /// Fetch the tail of a log file from Moonraker using HTTP Range requests
    static std::string fetch_log_tail(const std::string& base_url, const std::string& endpoint,
                                      int num_lines, int tail_bytes = 524288);

    /// Check if a key name matches a sensitive pattern
    static bool is_sensitive_key(const std::string& key);
};

} // namespace helix

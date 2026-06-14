// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <spdlog/spdlog.h>

#include <string>

namespace helix {
namespace logging {

/**
 * @brief Identifies which spdlog sink a pattern is being chosen for
 *
 * Each sink gets its own format: console/file carry an ms-precision timestamp
 * because nothing else stamps them, while journald/syslog/android rely on the
 * system clock and would double-stamp if we added our own time token. Every
 * sink includes the thread id (%t) — the single highest-value field for
 * diagnosing the main-thread-vs-background-thread confusion behind the
 * async-delete crash family.
 */
enum class SinkKind {
    Console,        ///< stdout color sink — ms timestamp + colored level + thread id
    File,           ///< rotating file sink — ms timestamp + level + thread id
    Journald,       ///< systemd journal — level + thread id (journal stamps time)
    Syslog,         ///< syslog — level + thread id (syslog stamps time)
    Android,        ///< Android logcat — thread id only (logcat adds metadata)
    CrashBreadcrumb ///< crash error-log ring — ms timestamp + level + thread id
};

/**
 * @brief Return the spdlog pattern string for a given sink kind
 *
 * Pure function (no spdlog/sink dependency) so the per-sink format decision is
 * unit-testable without constructing real sinks. Called once per sink right
 * after construction in init()/init_early(), via sink->set_pattern().
 *
 * Invariants enforced by tests/unit/test_log_pattern.cpp:
 *   - every pattern contains %t (thread id)
 *   - Console and File contain a time token; the system sinks do not
 *   - Console keeps the colored-level tokens %^ / %$
 *
 * @param kind Which sink the pattern is for
 * @return A static pattern string (valid for process lifetime)
 */
const char* pattern_for_sink(SinkKind kind);

/**
 * @brief Log destination targets
 *
 * On Linux, the system will auto-detect the best available target:
 * - Journal (systemd) if /run/systemd/journal/socket exists
 * - Syslog as fallback
 * - File as final fallback
 *
 * On macOS, only Console and File are available.
 */
enum class LogTarget {
    Auto,    ///< Detect best available (default)
    Journal, ///< systemd journal (Linux only)
    Syslog,  ///< Traditional syslog (Linux only)
    File,    ///< Rotating file log
    Console, ///< Console only (disable system logging)
    Android  ///< Android logcat via __android_log_print
};

/**
 * @brief Logging configuration
 */
struct LogConfig {
    spdlog::level::level_enum level = spdlog::level::warn;
    bool enable_console =
        true; ///< Enable console sink (only attached when target is Console or stdout is a TTY)
    LogTarget target = LogTarget::Auto; ///< System log destination
    std::string file_path;              ///< Override file path (empty = auto)
};

/**
 * @brief Initialize minimal logging for early startup
 *
 * Sets up a basic console logger at WARN level. Call this FIRST in main()
 * before any log calls. The full init() can reconfigure later with user
 * preferences from CLI args and config files.
 */
void init_early();

/**
 * @brief Initialize logging subsystem
 *
 * Call once at startup before any log calls. Creates a multi-sink logger
 * that writes to both console (if enabled) and the selected system target.
 *
 * @param config Logging configuration
 */
void init(const LogConfig& config);

/**
 * @brief Parse log target from string
 *
 * @param str One of: "auto", "journal", "syslog", "file", "console"
 * @return Corresponding LogTarget enum value (Auto if unrecognized)
 */
LogTarget parse_log_target(const std::string& str);

/**
 * @brief Get string name for log target
 *
 * @param target LogTarget enum value
 * @return Human-readable name (e.g., "journal", "syslog")
 */
const char* log_target_name(LogTarget target);

/**
 * @brief Human-readable description of the currently-active log destination
 *
 * Resolved during init() — reflects the effective target after Auto detection,
 * and for the File target returns the resolved file path. Suitable for display
 * in the About panel.
 *
 * Returns an empty string before init() has been called.
 */
std::string effective_destination();

/**
 * @brief The resolved file path the active file-sink writes to
 *
 * Single source of truth for "which file is the app logging to right now."
 * Returns the resolved path when the effective target is File, or an empty
 * string for every other target (journal, syslog, console, Android) and before
 * init() has run. Unlike effective_destination(), this never returns a
 * human-readable label — it is meant to be read back as an actual path. The
 * crash reporter and debug-bundle collector use it instead of re-deriving
 * candidate paths, so the two never diverge.
 */
std::string effective_log_file_path();

/**
 * @brief Parse log level from string
 *
 * @param str One of: "trace", "debug", "info", "warn", "warning", "error", "critical", "off"
 * @param default_level Level to return if string is empty or unrecognized
 * @return Corresponding spdlog level enum
 */
spdlog::level::level_enum
parse_level(const std::string& str, spdlog::level::level_enum default_level = spdlog::level::warn);

/**
 * @brief Convert CLI verbosity count to log level
 *
 * Maps: 0 -> warn, 1 -> info, 2 -> debug, 3+ -> trace
 *
 * @param verbosity Number of -v flags (0 = none)
 * @return Corresponding spdlog level
 */
spdlog::level::level_enum verbosity_to_level(int verbosity);

/**
 * @brief Convert spdlog level to libhv level
 *
 * libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)
 *
 * @param level spdlog log level
 * @return libhv log level integer
 */
int to_hv_level(spdlog::level::level_enum level);

/**
 * @brief Change log level at runtime (no restart needed)
 *
 * Updates both spdlog and libhv log levels immediately.
 * Call from the main thread when the user changes the log level setting.
 *
 * Not available in the watchdog build — the watchdog intentionally does not
 * link libhv, so runtime level changes for libhv's logger are not supported
 * there. The watchdog has its own static log level set at init.
 *
 * @param level New spdlog log level
 */
#ifndef HELIX_WATCHDOG
void set_runtime_level(spdlog::level::level_enum level);
#endif

/**
 * @brief Resolve log level with precedence: CLI > config > defaults
 *
 * @param cli_verbosity CLI -v flag count (0 = none)
 * @param config_level_str Log level from config file (empty = not set)
 * @param test_mode True if running in test mode (affects default)
 * @return Resolved log level
 */
spdlog::level::level_enum resolve_log_level(int cli_verbosity, const std::string& config_level_str,
                                            bool test_mode);

/**
 * @brief Tail of the in-memory ring-buffer log sink (newest-last, joined by \n)
 *
 * The ring buffer is installed on ALL platforms by init() and captures DEBUG
 * regardless of the user-configured level the file/syslog/console sinks run at.
 * It is the authoritative source for the debug bundle's log_tail because it is
 * always the live process and always fresh — unlike the file cascade, which on
 * syslog-target devices (AD5X/AD5M) falls back to stale leftover files and only
 * carries WARN-filtered /var/log/messages lines.
 *
 * Returns at most `num_lines` of the most-recent formatted log lines, oldest
 * first. Empty before init() has installed the sink (e.g. the watchdog build,
 * which does not call init() with a ring sink) or if nothing has been logged.
 *
 * @param num_lines Max lines to return (0 = all retained)
 * @return Newline-joined recent log lines, or empty string
 */
std::string tail_ring_buffer(int num_lines);

/// Number of messages the ring buffer currently retains (capacity), for the
/// bundle's log_meta diagnostic key. 0 before init() installs the sink.
size_t ring_buffer_capacity();

/// The effective spdlog level the persistent (file/syslog/console) sinks run
/// at — i.e. the user-configured level, NOT the ring buffer's debug floor.
/// Lets a bundle reader know whether debug was reaching persistent logs.
spdlog::level::level_enum effective_log_level();

} // namespace logging
} // namespace helix

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logging_init.h"

#ifndef HELIX_WATCHDOG
#include "hv/hlog.h"
#endif
#include "lvgl_assert_handler.h"
#include "lvgl_log_handler.h"
#ifndef HELIX_WATCHDOG
#include "system/crash_error_log_sink.h"
#include "system/crash_handler.h"
#endif

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <lvgl.h>
#include <memory>
#include <sstream>
#include <unistd.h>
#include <vector>

// Define the global callback pointer for LVGL assert handler
helix_assert_callback_t g_helix_assert_cpp_callback = nullptr;

#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
#include <spdlog/sinks/systemd_sink.h>
#endif
#include <spdlog/sinks/syslog_sink.h>
#endif

#ifdef HELIX_PLATFORM_ANDROID
#include <spdlog/sinks/android_sink.h>
#endif

namespace helix {
namespace logging {

const char* pattern_for_sink(SinkKind kind) {
    switch (kind) {
    case SinkKind::Console:
    case SinkKind::File:
        // ms timestamp, colored level (%^…%$ are no-ops on the non-color file
        // sink, so the same string is safe for both), thread id, message.
        return "[%H:%M:%S.%e] [%^%l%$] [%t] %v";
    case SinkKind::Journald:
    case SinkKind::Syslog:
        // No time token — journald/syslog stamp their own time. Keep the level
        // text (grep-ability of /var/log/messages) and the thread id.
        return "[%l] [%t] %v";
    case SinkKind::Android:
        // logcat already prefixes its own timestamp/level/tag metadata.
        return "[%t] %v";
    case SinkKind::CrashBreadcrumb:
        // Feeds crash context — keep a full line with thread id. The crash
        // ring reads msg.payload (the raw message), not this formatted output,
        // so the pattern is for any other consumer of this sink's stream.
        return "[%H:%M:%S.%e] [%l] [%t] %v";
    }
    return "[%t] %v";
}

namespace {

// Snapshot of the resolved log destination after init(), used by
// effective_destination() to surface the active sink in the About panel.
LogTarget g_effective_target = LogTarget::Auto;
std::string g_effective_file_path;

// Process-global handle to the in-memory ring-buffer sink. Installed on every
// platform by init() and read by tail_ring_buffer() (debug-bundle log_tail).
// Rebuilt on each init() — a shared_ptr so a logger swap never frees it out
// from under a concurrent tail read. Null in the watchdog build / before init.
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_ring_sink;

// The user-configured level the persistent sinks run at (the logger floor may
// be lower so the ring captures debug). Recorded for the bundle's log_meta.
spdlog::level::level_enum g_effective_log_level = spdlog::level::warn;

// Default ring capacity (messages). Tunable via HELIX_LOG_RING_LINES so a
// constrained device can shrink it. ~2000 lines ≈ a few hundred KB at typical
// line lengths — well within budget even on the 14 MB-diet AD5M.
constexpr size_t kDefaultRingLines = 2000;

size_t resolve_ring_capacity() {
    size_t lines = kDefaultRingLines;
    if (const char* env = std::getenv("HELIX_LOG_RING_LINES")) {
        char* end = nullptr;
        unsigned long long v = std::strtoull(env, &end, 10);
        if (end != env && v > 0) {
            lines = static_cast<size_t>(v);
        }
    }
    return lines;
}

// Whether the ring buffer captures DEBUG (the diagnostic win) or matches the
// persistent sinks' level (lower formatting cost). Default ON: the formatting
// cost of debug-level emission into a memory ring is modest even on MIPS/AD5X,
// and the bundle diagnostic value — recovering the live debug context that the
// WARN-level file/syslog sinks never persisted — is the entire point of this
// sink. Set HELIX_BUNDLE_LOG_DEBUG=0 to fall back to the configured level.
bool ring_captures_debug() {
    if (const char* env = std::getenv("HELIX_BUNDLE_LOG_DEBUG")) {
        return !(env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N');
    }
    return true;
}

/// Check if a path is writable (for file logging location selection)
bool is_path_writable(const std::string& path) {
    // Check parent directory for new files, or file itself if exists
    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();

    if (dir.empty()) {
        dir = ".";
    }

    // Check if directory exists and is writable
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return false;
    }

    // Try to determine write permission
    auto perms = std::filesystem::status(dir, ec).permissions();
    if (ec) {
        return false;
    }

    // Check owner write permission (simplified check)
    return (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
}

#ifndef HELIX_WATCHDOG
/// Non-owning shared_ptr to the process-lifetime crash error-log sink, so it
/// can join a logger's sink list without the logger ever freeing it (the sink
/// outlives every logger swap, keeping the crash-handler pointers valid).
spdlog::sink_ptr crash_error_log_sink() {
    auto& sink = CrashErrorLogSink::instance();
    sink.set_pattern(pattern_for_sink(SinkKind::CrashBreadcrumb));
    return spdlog::sink_ptr(&sink, [](spdlog::sinks::sink*) {});
}
#endif

/// Get XDG_DATA_HOME or default ~/.local/share
std::string get_xdg_data_home() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        return xdg;
    }

    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.local/share";
    }

    return "/tmp"; // Last resort fallback
}

/// Resolve log file path with fallback logic
std::string resolve_log_file_path(const std::string& override_path) {
    if (!override_path.empty()) {
        return override_path;
    }

    // Try /var/log first (requires permissions, typical for system services)
    const std::string var_log = "/var/log/helix-screen.log";
    if (is_path_writable(var_log)) {
        return var_log;
    }

    // Fallback to user directory
    std::string user_dir = get_xdg_data_home() + "/helix-screen";
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);

    return user_dir + "/helix.log";
}

/// Detect best available logging target at runtime
LogTarget detect_best_target() {
#ifdef HELIX_PLATFORM_ANDROID
    // Android: use logcat sink (stdout is invisible in adb logcat)
    return LogTarget::Android;
#elif defined(__linux__)
#ifdef HELIX_HAS_SYSTEMD
    // Check for systemd journal socket
    std::error_code ec;
    if (std::filesystem::exists("/run/systemd/journal/socket", ec)) {
        return LogTarget::Journal;
    }
#endif
    // Syslog is always available on Linux
    return LogTarget::Syslog;
#else
    // macOS/other: console only by default
    return LogTarget::Console;
#endif
}

/// Add system sink based on target
void add_system_sink(std::vector<spdlog::sink_ptr>& sinks, LogTarget target,
                     const std::string& file_path) {
    switch (target) {
#ifdef __linux__
#ifdef HELIX_HAS_SYSTEMD
    case LogTarget::Journal: {
        auto sink = std::make_shared<spdlog::sinks::systemd_sink_mt>("helix-screen");
        sink->set_pattern(pattern_for_sink(SinkKind::Journald));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::Syslog: {
        auto sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                    LOG_USER, false);
        sink->set_pattern(pattern_for_sink(SinkKind::Syslog));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::File: {
        std::string path = resolve_log_file_path(file_path);
        // Default: 5 MiB per file × 3 files (~15 MiB cap). Constrained-flash
        // platforms tune lower via HELIX_LOG_ROTATE_BYTES / _FILES env vars
        // (e.g., CC1 sets 1 MiB × 3 in hooks-cc1.sh).
        size_t max_bytes = 5 * 1024 * 1024;
        size_t max_files = 3;
        if (const char* env = std::getenv("HELIX_LOG_ROTATE_BYTES")) {
            char* end = nullptr;
            unsigned long long v = std::strtoull(env, &end, 10);
            if (end != env && v > 0) {
                max_bytes = static_cast<size_t>(v);
            }
        }
        if (const char* env = std::getenv("HELIX_LOG_ROTATE_FILES")) {
            char* end = nullptr;
            unsigned long long v = std::strtoull(env, &end, 10);
            if (end != env && v > 0) {
                max_files = static_cast<size_t>(v);
            }
        }
        auto sink =
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_bytes, max_files);
        sink->set_pattern(pattern_for_sink(SinkKind::File));
        sinks.push_back(std::move(sink));
        break;
    }
#ifdef HELIX_PLATFORM_ANDROID
    case LogTarget::Android: {
        auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("HelixScreen");
        sink->set_pattern(pattern_for_sink(SinkKind::Android));
        sinks.push_back(std::move(sink));
        break;
    }
#endif
    case LogTarget::Console:
    case LogTarget::Auto:
        // Console-only or auto (which would have been resolved already)
        // No additional sink needed
        break;
#ifdef __linux__
    default:
        // Handle Journal case when HELIX_HAS_SYSTEMD is not defined
        // Fall back to syslog
        if (target == LogTarget::Journal) {
            auto sink = std::make_shared<spdlog::sinks::syslog_sink_mt>("helix-screen", LOG_PID,
                                                                        LOG_USER, false);
            sink->set_pattern(pattern_for_sink(SinkKind::Syslog));
            sinks.push_back(std::move(sink));
        }
        break;
#else
    default:
        break;
#endif
    }
}

/// C++ assert callback that logs via spdlog and dumps backtrace.
/// IMPORTANT: Do NOT call any LVGL functions here — this callback may fire
/// during rendering or layout, and re-entrant LVGL calls cause cascading
/// assertions and SIGSEGV (crash signature 0997d072).
void lvgl_assert_spdlog_callback(const char* file, int line, const char* func) {
#ifndef HELIX_WATCHDOG
    // LVGL asserts log-and-continue (never abort), but leave a durable crumb:
    // if this assert later contributes to a crash, the breadcrumb names where
    // it fired (issue #987). Runs on the LVGL thread — satisfies breadcrumb's
    // single-producer contract.
    crash_handler::breadcrumb::note("assert", func);
#endif
    // Log via spdlog for consistent logging across all outputs
    spdlog::critical("╔═══════════════════════════════════════════════════════════╗");
    spdlog::critical("║              LVGL ASSERTION FAILED                        ║");
    spdlog::critical("╠═══════════════════════════════════════════════════════════╣");
    spdlog::critical("║ File: {}", file);
    spdlog::critical("║ Line: {}", line);
    spdlog::critical("║ Func: {}()", func);
    spdlog::critical("╚═══════════════════════════════════════════════════════════╝");

    // Dump recent log messages that led up to this assertion
    spdlog::critical("=== Recent log messages (backtrace) ===");
    spdlog::dump_backtrace();
}

} // namespace

void init_early() {
    // Create minimal console logger at WARN level so early startup code can log
    // without crashing. Attach the crash error-log sink too, so errors during
    // boot (before init()) are still captured for crash diagnostics (#987).
    std::vector<spdlog::sink_ptr> sinks;
    {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern(pattern_for_sink(SinkKind::Console));
        sinks.push_back(std::move(console));
    }
#ifndef HELIX_WATCHDOG
    sinks.push_back(crash_error_log_sink());
#endif
    auto logger = std::make_shared<spdlog::logger>("helix", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

void init(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;

    // Resolve auto-detection first so we can decide about console
    LogTarget effective_target =
        (config.target == LogTarget::Auto) ? detect_best_target() : config.target;

    // Snapshot for effective_destination() — recorded before sink construction
    // so the About panel reflects the same target the sinks are built from.
    g_effective_target = effective_target;
    g_effective_file_path =
        (effective_target == LogTarget::File) ? resolve_log_file_path(config.file_path) : "";

    // Console sink — added when enabled AND the target benefits from it.
    //
    // Behavior by target:
    //   - Console: console is the ONLY sink, always add.
    //   - Android: stdout is invisible; android_sink handles output. Skip.
    //   - Syslog/Journal/File: structured destination already captures output.
    //     Add console ONLY when stdout is a TTY (interactive run from a shell),
    //     so dev workstations and `ssh -t` sessions still see colored output.
    //     Daemonized launches under SysV/systemd have stdout redirected to a
    //     file or the journal — adding a stdout sink there double-logs every
    //     line (was the root cause of the Snapmaker U1 tmpfs blowout where
    //     /tmp/helixscreen.log grew to 498 MB at trace level).
    bool add_console = false;
    if (config.enable_console) {
        switch (effective_target) {
        case LogTarget::Console:
            add_console = true;
            break;
        case LogTarget::Android:
            add_console = false;
            break;
        default:
            add_console = (isatty(STDOUT_FILENO) != 0);
            break;
        }
    }
    if (add_console) {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern(pattern_for_sink(SinkKind::Console));
        sinks.push_back(std::move(console));
    }

    // The console/system sinks emit only at the user-configured level so
    // persistent logs (/var/log/messages, the journal, the console) keep their
    // normal volume — no spam, no tmpfs blowout. The logger floor is raised
    // below (to debug) so the ring buffer alone gets the extra detail.
    g_effective_log_level = config.level;
    for (auto& s : sinks) {
        s->set_level(config.level);
    }

    // Add system sink (also at the configured level — set immediately after).
    {
        size_t before = sinks.size();
        add_system_sink(sinks, effective_target, config.file_path);
        for (size_t i = before; i < sinks.size(); ++i) {
            sinks[i]->set_level(config.level);
        }
    }

    // In-memory ring-buffer sink — installed on ALL platforms. This is the
    // authoritative source for the debug bundle's log_tail: always the live
    // process, always fresh, and (by default) always carrying DEBUG even when
    // the persistent sinks run at WARN. On syslog-target devices (AD5X/AD5M)
    // the file cascade otherwise falls back to a stale leftover file and only
    // WARN-filtered /var/log/messages lines reach the bundle — the runtime
    // debug context needed to diagnose an in-progress incident (e.g. a stuck
    // IFS filament purge) was being lost entirely.
    //
    // Perf tradeoff (MIPS/AD5X): debug-level emission costs a format pass per
    // line into the ring even when persistent sinks drop it. That cost is
    // bounded (memory ring, no I/O) and justified by the bundle's diagnostic
    // value; HELIX_BUNDLE_LOG_DEBUG=0 reverts the ring to the configured level.
    const spdlog::level::level_enum ring_level =
        ring_captures_debug() ? spdlog::level::debug : config.level;
    g_ring_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(resolve_ring_capacity());
    g_ring_sink->set_pattern(pattern_for_sink(SinkKind::File));
    g_ring_sink->set_level(ring_level);
    sinks.push_back(g_ring_sink);

#ifndef HELIX_WATCHDOG
    // Always retain recent ERROR-level lines for crash diagnostics, regardless
    // of the output target (#987 last-ditch reason capture). Its own level is
    // left at the sink default (trace) so it never misses an error.
    sinks.push_back(crash_error_log_sink());
#endif

    // Create logger with all sinks. The logger level gates messages BEFORE any
    // sink sees them, so it must be the MORE VERBOSE of {configured level, ring
    // level} — otherwise debug lines are dropped at the logger and never reach
    // the ring. Per-sink levels (set above) then restore each persistent sink's
    // normal volume; only the ring buffer gains the extra detail.
    auto logger = std::make_shared<spdlog::logger>("helix", sinks.begin(), sinks.end());
    logger->set_level(std::min(config.level, ring_level));

    // Set as default logger
    spdlog::set_default_logger(logger);

    // Enable backtrace buffer to capture recent log messages before an assertion
    // These get dumped when spdlog::dump_backtrace() is called in the assert handler
    spdlog::enable_backtrace(32);

    // Register C++ callback for LVGL assert handler
    // This provides spdlog integration and LVGL state context
    g_helix_assert_cpp_callback = lvgl_assert_spdlog_callback;

    // NOTE: LVGL log handler is registered separately AFTER lv_init()
    // because lv_init() resets the global state and clears any callbacks.
    // See Application::init_display() which calls register_lvgl_log_handler().

    // Log what we configured (at debug level so it's not noisy)
    spdlog::debug("[Logging] Initialized: target={}, console={}, backtrace=32 messages",
                  log_target_name(effective_target), config.enable_console ? "yes" : "no");
}

LogTarget parse_log_target(const std::string& str) {
    if (str == "journal")
        return LogTarget::Journal;
    if (str == "syslog")
        return LogTarget::Syslog;
    if (str == "file")
        return LogTarget::File;
    if (str == "console")
        return LogTarget::Console;
    if (str == "android")
        return LogTarget::Android;
    return LogTarget::Auto; // Default for "auto" or unrecognized
}

std::string effective_destination() {
    switch (g_effective_target) {
    case LogTarget::Journal:
        return "systemd journal";
    case LogTarget::Syslog:
        return "syslog";
    case LogTarget::File:
        return g_effective_file_path;
    case LogTarget::Console:
        return "console";
    case LogTarget::Android:
        return "Android logcat";
    case LogTarget::Auto:
        return "";
    }
    return "";
}

std::string effective_log_file_path() {
    return (g_effective_target == LogTarget::File) ? g_effective_file_path : std::string{};
}

std::string tail_ring_buffer(int num_lines) {
    auto sink = g_ring_sink; // copy the shared_ptr so a concurrent init() swap is safe
    if (!sink) {
        return {};
    }
    size_t lim = num_lines > 0 ? static_cast<size_t>(num_lines) : 0;
    auto lines = sink->last_formatted(lim); // oldest-first, already formatted
    if (lines.empty()) {
        return {};
    }
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out << '\n';
        }
        // last_formatted() appends the pattern's trailing newline; strip it so
        // join produces one clean newline between entries (not a blank line).
        std::string& line = lines[i];
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }
        out << line;
    }
    return out.str();
}

size_t ring_buffer_capacity() {
    return g_ring_sink ? resolve_ring_capacity() : 0;
}

spdlog::level::level_enum effective_log_level() {
    return g_effective_log_level;
}

const char* log_target_name(LogTarget target) {
    switch (target) {
    case LogTarget::Auto:
        return "auto";
    case LogTarget::Journal:
        return "journal";
    case LogTarget::Syslog:
        return "syslog";
    case LogTarget::File:
        return "file";
    case LogTarget::Console:
        return "console";
    case LogTarget::Android:
        return "android";
    }
    return "unknown";
}

spdlog::level::level_enum parse_level(const std::string& str,
                                      spdlog::level::level_enum default_level) {
    if (str.empty()) {
        return default_level;
    }
    if (str == "trace") {
        return spdlog::level::trace;
    }
    if (str == "debug") {
        return spdlog::level::debug;
    }
    if (str == "info") {
        return spdlog::level::info;
    }
    if (str == "warn" || str == "warning") {
        return spdlog::level::warn;
    }
    if (str == "error") {
        return spdlog::level::err;
    }
    if (str == "critical") {
        return spdlog::level::critical;
    }
    if (str == "off") {
        return spdlog::level::off;
    }
    return default_level;
}

spdlog::level::level_enum verbosity_to_level(int verbosity) {
    if (verbosity <= 0) {
        return spdlog::level::warn;
    }
    switch (verbosity) {
    case 1:
        return spdlog::level::info;
    case 2:
        return spdlog::level::debug;
    default:
        return spdlog::level::trace;
    }
}

int to_hv_level(spdlog::level::level_enum level) {
    // libhv levels: VERBOSE(0) < DEBUG(1) < INFO(2) < WARN(3) < ERROR(4) < FATAL(5) < SILENT(6)
    switch (level) {
    case spdlog::level::trace:
    case spdlog::level::debug:
        return 1; // LOG_LEVEL_DEBUG (libhv has no trace, cap at debug)
    case spdlog::level::info:
        return 2; // LOG_LEVEL_INFO
    case spdlog::level::warn:
        return 3; // LOG_LEVEL_WARN
    case spdlog::level::err:
        return 4; // LOG_LEVEL_ERROR
    case spdlog::level::critical:
        return 5; // LOG_LEVEL_FATAL
    case spdlog::level::off:
        return 6; // LOG_LEVEL_SILENT
    default:
        return 3; // LOG_LEVEL_WARN
    }
}

#ifndef HELIX_WATCHDOG
void set_runtime_level(spdlog::level::level_enum level) {
    // Mirror init()'s split: the logger floor must stay at least as verbose as
    // the ring buffer so debug keeps reaching it, while each persistent sink
    // (and libhv) moves to the user-requested level. A plain spdlog::set_level()
    // would set the logger floor to `level` and starve the ring of debug when
    // the user picks WARN — defeating the always-on debug-capture fix.
    const spdlog::level::level_enum ring_level =
        ring_captures_debug() ? spdlog::level::debug : level;
    g_effective_log_level = level;

    // The crash error-log sink must keep capturing ERROR regardless of the
    // user's level choice (#987), so it is excluded from the per-sink retune.
    spdlog::sink_ptr crash_sink;
#ifndef HELIX_WATCHDOG
    crash_sink = crash_error_log_sink();
#endif

    if (auto logger = spdlog::default_logger()) {
        for (auto& s : logger->sinks()) {
            if (s == g_ring_sink) {
                // Leave the ring at its debug floor so bundles keep debug detail.
                s->set_level(ring_level);
            } else if (crash_sink && s.get() == crash_sink.get()) {
                // Crash breadcrumb sink: never raise above error (#987).
                s->set_level(std::min(s->level(), spdlog::level::err));
            } else {
                s->set_level(level);
            }
        }
        logger->set_level(std::min(level, ring_level));
    } else {
        spdlog::set_level(std::min(level, ring_level));
    }

    hlog_set_level(to_hv_level(level));
    spdlog::info("[Logging] Runtime log level changed to {}",
                 spdlog::level::to_string_view(level).data());
}
#endif

spdlog::level::level_enum resolve_log_level(int cli_verbosity, const std::string& config_level_str,
                                            bool test_mode) {
    // Precedence: CLI verbosity > config file > defaults

    // CLI verbosity takes top precedence
    if (cli_verbosity > 0) {
        return verbosity_to_level(cli_verbosity);
    }

    // Config file level (if specified)
    if (!config_level_str.empty()) {
        // Use warn as fallback for invalid config strings
        return parse_level(config_level_str, spdlog::level::warn);
    }

    // Defaults: test mode = debug, production = warn
    return test_mode ? spdlog::level::debug : spdlog::level::warn;
}

} // namespace logging
} // namespace helix

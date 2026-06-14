# Logging Guidelines

This document defines the logging standards for HelixScreen. All new code should follow these patterns; existing code will be migrated incrementally.

## Log Levels

| Level | CLI Flag | Purpose | Examples |
|-------|----------|---------|----------|
| **ERROR** | (always) | Unrecoverable failures | "Failed to connect", "NULL pointer" |
| **WARN** | (always) | Recoverable issues, guards | "Double init detected", "Fallback used" |
| **INFO** | `-v` | User-visible milestones | "Connected to Moonraker", "Setup complete" |
| **DEBUG** | `-vv` | Troubleshooting info | Component init summaries, state changes |
| **TRACE** | `-vvv` | Wire-level details | Per-item loops, JSON-RPC protocol, observer plumbing |

## When to Use Each Level

### ERROR
- Unrecoverable failures that require user action
- NULL pointer dereferences (guarded)
- Missing critical configuration
- Failed operations that cannot proceed

### WARN
- Recoverable issues with automatic fallbacks
- Guard checks (double initialization, missing optional components)
- Deprecated API usage
- Situations that might indicate a problem but don't prevent operation

### INFO
Use INFO sparingly for **milestones the user cares about**:
- System startup completion: "HelixScreen UI Prototype"
- Connection events: "✓ Connected to Moonraker"
- Discovery summaries: "Capabilities: QGL, bed_mesh, chamber_sensor"
- Panel setup completion: "[Home Panel] Setup complete!"
- Major operations: "File list updated: 126 G-code files"

**NOT INFO** (use DEBUG or TRACE instead):
- Navigation events: "Switched to panel 2" → DEBUG
- Mock backend operations: "Returning mock file list" → DEBUG
- Per-item updates: "Updated slot 0 info" → DEBUG
- Internal wiring: "Queuing switch to panel" → DEBUG

### DEBUG
Use DEBUG for **troubleshooting information**:
- Component initialization summaries: "Subjects initialized"
- Configuration details: "Target: 800x480, DPI: 160"
- State change summaries: "Printer connection state changed: Connected"
- Batch operation summaries: "Auto-registered 21 theme-aware color pairs"
- Discovery details: "Detected probe: probe"

**NOT DEBUG** (use TRACE instead):
- Per-widget XML apply: "Applied size preset: 64x32" → TRACE
- Per-file metadata: "Using cached thumbnail: X.png" → TRACE
- RPC method calls: "Mock send_jsonrpc: method" → TRACE
- File listing results: "Found 11 files", "Directory has X items" → TRACE
- UI state toggles: "Overlay backdrop visibility set to: true" → TRACE
- Spoolman lookups: "get_spoolman_spool(1) -> Polymaker PLA" → TRACE
- Theme file parsing: "Parsing X.json in legacy format" → TRACE

### TRACE
Use TRACE for **deep debugging only**:
- Per-item processing in loops: "Registering color graph_bg: selected=#2D2D2D"
- Wire protocol: "send_jsonrpc: {...}", "Registered request 5 for method X"
- Observer/callback registration: "Registering observer on X at 0x..."
- Subject value changes: "Subject value now: 2"
- Per-widget creation: "Created widget for slot 3"

## Message Format

### Prefix Standard: `[ComponentName]`

**Correct:**
```cpp
spdlog::debug("[Theme] Auto-registered 21 color pairs");
spdlog::info("[Home Panel] Setup complete!");
spdlog::trace("[Moonraker Client] send_jsonrpc: {}");
```

**Incorrect:**
```cpp
spdlog::debug("Theme: Auto-registered 21 color pairs");  // NO colon
spdlog::info("[Home Panel]: Setup complete!");           // NO double colon
spdlog::debug("Home Panel] Setup complete!");            // Missing bracket
```

### Prefix Naming Rules

| Pattern | Use For | Example |
|---------|---------|---------|
| `[ClassName]` | Classes | `[MoonrakerClient]`, `[PrinterState]` |
| `[Feature Name]` | Multi-word features | `[Home Panel]`, `[Print Select Panel]` |
| `[Subsystem]` | Subsystems | `[Theme]`, `[AMS AFC]` |

### Message Content

- **Be specific**: Include relevant data (counts, states, identifiers)
- **Be consistent**: Use the same wording for similar events
- **Be actionable**: For errors/warnings, include what went wrong

**Good:**
```cpp
spdlog::info("[PrinterState] Initialized 6 fans (version 1)");
spdlog::debug("[Controls Panel] Populated 3 secondary fans");
spdlog::trace("[Theme] Registering spacing space_lg: selected=16");
```

**Bad:**
```cpp
spdlog::info("fans initialized");  // No context
spdlog::debug("Done");             // Not actionable
```

## Loop Logging Pattern

When logging inside loops, use TRACE for per-item and DEBUG for summaries:

```cpp
int registered = 0;
for (const auto& item : items) {
    spdlog::trace("[Theme] Registering {}: value={}", item.name, item.value);
    // ... registration logic ...
    registered++;
}
spdlog::debug("[Theme] Auto-registered {} items", registered);
```

## Common Patterns

### Initialization Guards
```cpp
if (initialized_) {
    spdlog::warn("[MyComponent] init_subjects() called twice - ignoring");
    return;
}
// ... initialization ...
spdlog::debug("[MyComponent] Subjects initialized");
```

### Error Handling
```cpp
if (!ptr) {
    spdlog::error("[MyComponent] Cannot process: NULL pointer");
    return;
}
```

### Milestone Completion
```cpp
// After significant setup work
spdlog::info("[MyComponent] Setup complete!");
```

### Wire Protocol
```cpp
spdlog::trace("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
spdlog::trace("[Moonraker Client] Registered request {} for method {}", id, method);
```

## Implementation Notes

- Use `spdlog` exclusively (not `printf`, `std::cout`, or `LV_LOG_*`)
- Every log line carries the emitting **thread id** and (on console/file) an ms timestamp — see "Per-Sink Log Patterns" below
- For errors that should notify the user, use `NOTIFY_ERROR()` macro

---

# Log Destinations & Retrieval

There are **two independent log streams** on every deployment:

1. **Structured app log** — every `spdlog::*` call from helix-screen, helix-watchdog, helix-splash. Routed through spdlog sinks.
2. **Launcher subshell capture** — `helix-launcher.sh` echoes, libc/glibc abort output, anything written to stdout/stderr before `init_logging()` runs. Captured by the SysV init script's `>> "$LOGFILE" 2>&1` redirect. Never sees per-line spdlog output (see "Console sink" below).

These two streams do **not** overlap. spdlog never writes to the launcher file (in daemon mode); the shell redirect never sees structured spdlog calls. They are complementary diagnostics, not redundant.

## spdlog Sinks & Auto-Detection

`Application::init_logging()` builds a spdlog logger with one or more sinks. Implementation: `src/system/logging_init.cpp`.

### Target Resolution

The runtime target is set by precedence (highest → lowest):

1. `--log-dest=<dest>` CLI flag (passed through by `helix-launcher.sh`)
2. `HELIX_LOG_DEST` env var (read by launcher)
3. `/log_dest` in `settings.json`
4. `LogTarget::Auto` (default) → resolved by `detect_best_target()`:
   - **`HELIX_PLATFORM_ANDROID`** → `Android` (`__android_log_print`)
   - **Linux with `HELIX_HAS_SYSTEMD` AND `/run/systemd/journal/socket` exists** → `Journal` (`systemd_sink_mt`)
   - **Other Linux** → `Syslog` (`syslog_sink_mt` via `libc syslog()`)
   - **macOS/other** → `Console` (`stdout_color_sink_mt`)

Valid `LogTarget` values: `auto`, `journal`, `syslog`, `file`, `console`, `android`.

### Per-Sink Log Patterns

Each sink gets its **own** spdlog pattern (applied via `sink->set_pattern()` right after construction — not a global `spdlog::set_pattern()`, since the formats differ). The pattern strings come from the pure helper `helix::logging::pattern_for_sink(SinkKind)` in `logging_init.h`, so the format decision is unit-testable without constructing real sinks (`tests/unit/test_log_pattern.cpp`, tag `[logging][pattern]`).

| Sink (`SinkKind`) | Pattern | Notes |
|---|---|---|
| Console (`stdout_color_sink`) | `[%H:%M:%S.%e] [%^%l%$] [%t] %v` | ms timestamp, colored level, thread id |
| File (`rotating_file_sink`) | `[%H:%M:%S.%e] [%^%l%$] [%t] %v` | same string — `%^…%$` are no-ops on the non-color file sink |
| journald (`systemd_sink`) | `[%l] [%t] %v` | **no time token** — journald stamps its own time |
| syslog (`syslog_sink`) | `[%l] [%t] %v` | **no time token** — syslog stamps its own; `%l` kept for grep-ability of `/var/log/messages` |
| Android (`android_sink`) | `[%t] %v` | logcat adds its own timestamp/level/tag metadata |
| Crash breadcrumb (`CrashErrorLogSink`) | `[%H:%M:%S.%e] [%l] [%t] %v` | feeds crash context; the ring actually stores `msg.payload`, so this pattern is for any other consumer of the stream |

**Why the thread id (`%t`) is on every sink:** the worst crash family in this codebase (L081 / async-delete) is about main-thread-vs-background-thread (WebSocket / HTTP worker) confusion. Knowing which thread emitted a line is the single highest-value field for diagnosing it. The `[logging][pattern]` test fails if `%t` is dropped from any sink or if a time token is added to the system sinks (which would double-stamp the journal/syslog clock).

A console/file line now looks like:

```
[14:32:07.918] [debug] [140351827234560] [PrinterState] Initialized 6 fans (version 1)
```

### Console Sink (Stdout) — When It's Attached

The console sink is **opt-in by detection**. Logic in `logging_init.cpp::init()`:

| Resolved target | Console sink? |
|---|---|
| `Console` | Always — it's the only sink |
| `Android` | Never — stdout is invisible to logcat |
| `Syslog` / `Journal` / `File` | **Only when `isatty(STDOUT_FILENO)`** |

The TTY check means:
- Dev workstation running `./build/bin/helix-screen --test -vv` from a shell → console sink on, colored output to terminal.
- `ssh -t pi 'helix-screen ...'` → tty allocated → console sink on.
- SysV daemon launch where stdout is redirected to a file → not a tty → console sink **off**.
- systemd daemon launch where stdout is captured by journald → not a tty → console sink off (journald already has the structured stream via the journal sink).

This prevents the "double-log" mode that caused the Snapmaker U1 print failure where spdlog at trace wrote ~35 lines/sec to stdout, the init script captured stdout to a tmpfs file, and 498 MB filled `/tmp`.

### Reading `--test` Logs When stdout Isn't a TTY

The same `isatty(STDOUT_FILENO)` gate bites dev runs too: launch `./build/bin/helix-screen --test` with stdout **redirected or piped** (background run, `> log.txt`, `| grep`, a non-interactive agent shell) and there's no TTY, so the **console sink is not attached** — nothing prints to the pipe. The logs still land in the resolved system sink (syslog/journal on Linux dev boxes), not on stdout.

Read them from syslog instead:

```bash
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen --test -v &   # boots, runs headless
sleep 6 && kill %1
journalctl --since "1 min ago" | grep helix          # all lines
journalctl --since "1 min ago" | grep '\[PrinterDetector\]'   # one subsystem
```

Run it from an interactive shell (or `ssh -t`) and the console sink **is** attached — output goes straight to the terminal as usual.

Don't reach for `--log-dest file` to work around this: it writes `/var/log/helix-screen.log`, which is **not writable by a non-root user**, so the run fails. For redirected/background dev runs, syslog (`journalctl`) is the way.

Verbosity still applies (`-v`=info, `-vv`=debug, `-vvv`=trace). Detection lines such as `[PrinterState] Printer type set to: '…'` are **info-level**, so `-v` is enough to see them.

### Per-Platform Routing Summary

| Platform | spdlog target (default) | Where structured logs land | How to read |
|---|---|---|---|
| Raspberry Pi (systemd) | Journal | systemd journal | `journalctl -u helixscreen -f` |
| x86/x86_64 (systemd) | Journal | systemd journal | `journalctl -u helixscreen -f` |
| Snapmaker U1 (Debian Trixie, SysV) | Syslog | `/var/log/messages` (rsyslogd persists on overlay) | `grep helix-screen /var/log/messages` or `journalctl -t helix-screen` if journald-only |
| AD5M Forge-X/KMod (BusyBox SysV) | Syslog | `/var/log/messages` (BusyBox syslogd) | `grep helix-screen /var/log/messages` |
| AD5X (ZMOD MIPS, BusyBox) | Syslog | `/var/log/messages` | `grep helix-screen /var/log/messages` |
| Creality K1/K1C (BusyBox, in-memory syslog) | Syslog | in-memory ring buffer | `logread \| grep helix-screen` |
| Creality K2 (BusyBox procd) | Syslog | in-memory ring buffer | `logread \| grep helix-screen` |
| Elegoo CC1 / COSMOS (BusyBox) | Syslog | in-memory ring buffer | `logread \| grep helix-screen` |
| SonicPad (Debian) | Syslog | `/var/log/syslog` | `grep helix-screen /var/log/syslog` |
| Android | Android | logcat | `adb logcat -s HelixScreen` |
| Dev workstation (macOS / interactive Linux) | Console | stdout in terminal | visible directly |

## Launcher Subshell Capture (`launcher.log`)

`config/helixscreen.init` runs the launcher in a backgrounded subshell with `( ... exec "$LAUNCHER" ) >> "$LOGFILE" 2>&1 &`. Everything written to stdout/stderr inside that subshell ends up in `$LOGFILE`:

- `[helix-launcher] ...` shell echoes (binary selection, splash detection, log level, exit code)
- glibc abort output: `*** glibc detected ***`, `MALLOC_CHECK_=3` diagnostics, `std::terminate without active exception` — happens **after** spdlog is dead and can be the only diagnostic on a crash
- Early-init spdlog console output before `Application::init_logging()` swaps to the real logger (a handful of lines, warn-level)
- LVGL warnings emitted before the LVGL log handler is wired up

**It is NOT the structured app log.** For that, use the syslog/journal commands above.

### Path resolution

`LOGFILE` is chosen at script-start by `config/helixscreen.init`:

1. Probe `/var/log` — writable AND its backing filesystem (from `/proc/mounts`) isn't `tmpfs` or `ramfs` → `LOGFILE="/var/log/helixscreen/launcher.log"` (the FHS-conforming location).
2. Otherwise (tmpfs/ramfs `/var/log`, or `/var/log` not writable) → `LOGFILE="${DAEMON_DIR}/logs/launcher.log"` where `DAEMON_DIR` is the install directory (always persistent ext4 on supported targets).

| Platform | Resolved path |
|---|---|
| Raspberry Pi | `/var/log/helixscreen/launcher.log` (systemd platforms don't use this script — uses `ExecStart` via systemd journal instead) |
| Snapmaker U1 | `/var/log/helixscreen/launcher.log` (overlay's upperdir is persistent ext4) |
| AD5M Forge-X/KMod | `/opt/helixscreen/logs/launcher.log` (`/var/log` is tmpfs on BusyBox) |
| K1 / K1C | `/usr/data/helixscreen/logs/launcher.log` |
| K2 | `/usr/data/helixscreen/logs/launcher.log` |
| AD5X (ZMOD) | `/usr/data/helixscreen/logs/launcher.log` (ghzserg's S80 also redirects to `/opt/config/mod_data/log/helixscreen.log`) |
| CC1 / COSMOS | `/user-resource/helixscreen/logs/launcher.log` |

Size is capped at 5 MB at every `start` (the init script truncates if larger). This is belt-and-suspenders; the file shouldn't grow that big now that the console sink is gated off in daemon mode.

### Pre-v0.99.62 (legacy) path

Older installs wrote to `/tmp/helixscreen.log` on every SysV target. Debug bundles still scan this path for backward compatibility (`src/system/log_collector.cpp:default_file_paths()`).

## Controlling Log Verbosity

### CLI

| Flag | Effect |
|---|---|
| `-v` | INFO and above |
| `-vv` | DEBUG and above |
| `-vvv` | TRACE and above |
| `--log-level=trace\|debug\|info\|warn\|error\|critical\|off` | Explicit level (overrides `-v`) |
| `--log-dest=auto\|journal\|syslog\|file\|console\|android` | Override destination |
| `--log-file=<path>` | Override path when `--log-dest=file` |

### Env / config

`helixscreen.env` (in `<install_dir>/config/`, but on Klipper platforms it's symlinked to `~/printer_data/config/helixscreen/helixscreen.env`):

```
HELIX_LOG_LEVEL=warn   # trace, debug, info, warn, error, critical, off
HELIX_LOG_DEST=auto    # auto, journal, syslog, file, console
HELIX_LOG_FILE=        # path; only used when HELIX_LOG_DEST=file
```

Precedence: `--log-*` CLI flag > `HELIX_LOG_*` env > `/log_*` in `settings.json` > defaults (production: `warn`; test mode: `debug`).

### Runtime (in-app)

The Settings → System → Log Level dropdown calls `helix::logging::set_runtime_level()`, which is `spdlog::set_level()` globally + persists to `settings.json`. This survives restart via the precedence chain (config-file level applies on next launch if no CLI/env override is set).

### Klipper-style helixscreen.env convention

On Klipper-based platforms (Pi, AD5M, K1, K2, Snapmaker U1, etc.), `setup_config_symlink()` in `scripts/lib/installer/platform.sh` creates the canonical env file at `~/printer_data/config/helixscreen/helixscreen.env` (visible/editable from Mainsail/Fluidd) and symlinks `<install_dir>/config/helixscreen.env` → that path. Edit either location; both point to the same file. The launcher only reads the install-dir copy, so a broken symlink silently makes user edits invisible — `make deploy-<platform>` now verifies and repairs the symlink during deploy.

## Debug Bundles

`src/system/log_collector.cpp` assembles a debug bundle on user request (Settings → About → Generate Debug Bundle, or the `helix_debug` Moonraker shell command). It captures:

- Last N lines from each candidate launcher-log path (`/var/log/helixscreen/launcher.log`, `${install_dir}/logs/launcher.log`, legacy `/tmp/helixscreen.log`, etc.)
- Last N lines of syslog (`/var/log/messages`, `/var/log/syslog`)
- systemd journal entries when available (`journalctl -u helixscreen`)
- Crash report (if recent)
- `settings.json` (sanitized)

Bundles are uploaded to `crash.helixscreen.org` with a 6-char share code. Retrieval: `./scripts/debug-bundle.sh <CODE> --save`.

---

## Related Documentation

- `DEVELOPMENT.md#contributing` - Code standards
- `DEVELOPMENT.md` - Build and debug workflow
- `CLAUDE.md` - AI assistant rules (includes verbosity flags)
- `ENVIRONMENT_VARIABLES.md` - All `HELIX_*` env vars including `HELIX_LOG_LEVEL`
- `CRASH_REPORTER.md` - How crashes are captured and shipped
- `../user/TROUBLESHOOTING.md` - User-facing guide to reading logs

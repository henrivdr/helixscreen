#!/bin/sh
# scripts/lib/installer/recovery.sh
# Per-platform deep-recovery shell_command installation.
#
# HelixScreen's frontend calls Moonraker's `[shell_command helix_recover]` when
# the user asks for a "deep restart" (or when we auto-trigger after detecting
# `key298: Can not update MCU rpi config as it is shutdown`). The C++ side
# stays platform-blind — every platform that needs more than `firmware_restart`
# ships a moonraker.conf snippet here.
#
# Platforms that don't need this (stock Klipper / Bambu / RatOS Pi) get nothing:
# the recovery service falls back to `printer.firmware_restart` automatically.

# Re-source guard
[ -n "${_HELIX_RECOVERY_SOURCED:-}" ] && return 0
_HELIX_RECOVERY_SOURCED=1

# Snippet body for Creality K2 series (K2 Plus / K2 Pro / K2 Max).
#
# K2 runs Klipper as two host-side processes: `klippy.py` and `klipper_mcu`
# (a separate daemon serving as the rpi virtual MCU for the RS-485 bridge to
# the CFS unit). When the CFS unit raises an error severe enough to shut down
# its MCU side, FIRMWARE_RESTART alone can't recover — Klipper reconnects, sees
# rpi MCU still locked, and re-halts with `key298`.
#
# Recovery is: bounce the klipper_mcu daemon first, then klipper.
helix_recover_snippet_k2() {
    cat <<'EOF'

[shell_command helix_recover]
# HelixScreen deep-recovery (managed). See:
# https://github.com/prestonbrown/helixscreen/blob/main/scripts/lib/installer/recovery.sh
command: sh -c "/etc/init.d/klipper_mcu restart 2>&1; sleep 2; /etc/init.d/klipper restart 2>&1"
timeout: 30
verbose: True
EOF
}

# Probe the running Moonraker to see if its `shell_command` component is
# loaded. The component ships with mainline Moonraker and is auto-loaded when
# *any* `[shell_command]` block is present — but stripped/forked builds may
# omit it (e.g. some vendor-locked firmwares). We treat absence as "this
# platform can't host helix_recover" rather than blindly writing a section
# Moonraker would warn about on every restart.
#
# Heuristic: GET /server/info, look for "shell_command" in components[].
# Returns 0 (true) if the component is present, 1 if absent, 2 if probe failed
# (treat as "unknown — proceed" so an offline install doesn't block).
moonraker_has_shell_command_component() {
    # Resolve moonraker URL — defaults to localhost; respects HELIX_MOONRAKER_URL.
    local url="${HELIX_MOONRAKER_URL:-http://127.0.0.1:7125}"

    # Prefer curl, fall back to wget. K2's busybox wget can't do HTTPS but our
    # endpoint is HTTP-only, so it's fine here.
    local body
    if command -v curl >/dev/null 2>&1; then
        body=$(curl -sf --max-time 3 "$url/server/info" 2>/dev/null) || return 2
    elif command -v wget >/dev/null 2>&1; then
        body=$(wget -qO- --timeout=3 "$url/server/info" 2>/dev/null) || return 2
    else
        return 2
    fi

    # Substring match against the JSON. We don't have jq on every target, and
    # parsing JSON in pure POSIX shell is more pain than it's worth here.
    case "$body" in
        *'"shell_command"'*) return 0 ;;
        *) return 1 ;;
    esac
}

# True iff the given moonraker.conf already has our marker block.
has_recovery_section() {
    local conf="$1"
    [ -f "$conf" ] && grep -q '^\[shell_command helix_recover\]' "$conf" 2>/dev/null
}

# Return the snippet body for the detected platform, or empty when this
# platform doesn't need a custom recovery shell_command (firmware_restart is
# sufficient).
helix_recover_snippet_for_platform() {
    local platform="$1"
    case "$platform" in
        k2)   helix_recover_snippet_k2 ;;
        # k1c)  helix_recover_snippet_k1c ;;     # TBD
        # ad5m) helix_recover_snippet_ad5m ;;    # TBD
        # ad5x) helix_recover_snippet_ad5x ;;    # TBD
        # cc1)  helix_recover_snippet_cc1 ;;     # TBD
        *)    : ;; # nothing — frontend falls back to firmware_restart
    esac
}

# Install or update the helix_recover shell_command in moonraker.conf for the
# detected platform. Idempotent: re-running on an already-configured host is a
# no-op apart from a debug log line. Non-K2-style platforms with no snippet
# silently skip.
install_recovery_section() {
    local conf="$1"
    local platform="$2"
    local fs="${3:-}"   # `sudo` prefix if needed, "" otherwise — match moonraker.sh idiom

    [ -z "$conf" ] && return 1
    [ -z "$platform" ] && return 1

    local snippet
    snippet=$(helix_recover_snippet_for_platform "$platform")
    if [ -z "$snippet" ]; then
        log_info "No deep-recovery shell_command needed for platform: $platform"
        return 0
    fi

    if has_recovery_section "$conf"; then
        log_info "[shell_command helix_recover] already in $conf — skipping"
        return 0
    fi

    log_info "Adding [shell_command helix_recover] to $conf for platform: $platform"
    printf '%s\n' "$snippet" | $fs tee -a "$conf" >/dev/null
    log_success "Added [shell_command helix_recover] block"
}

# Remove our managed block on uninstall. Strips from `[shell_command helix_recover]`
# down to (but not including) the next `[section]` or EOF.
remove_recovery_section() {
    local conf="$1"
    local fs="${2:-}"

    [ -f "$conf" ] || return 0
    has_recovery_section "$conf" || return 0

    log_info "Removing [shell_command helix_recover] from $conf"
    local tmp
    tmp=$(mktemp)
    awk '
        /^\[shell_command helix_recover\]/ { skip=1; next }
        skip && /^\[/ { skip=0 }
        !skip { print }
    ' "$conf" > "$tmp"
    $fs cp "$tmp" "$conf"
    rm -f "$tmp"
    log_success "Removed [shell_command helix_recover] block"
}

# High-level installer entry point — drop-in companion to
# configure_moonraker_updates() in moonraker.sh. Call from main install flow
# AFTER configure_moonraker_updates so the conf already exists.
configure_moonraker_recovery() {
    local platform="$1"

    # Don't probe further if this platform has no snippet — keeps logs quiet
    # for stock Klipper / Bambu / RatOS Pi etc.
    local snippet
    snippet=$(helix_recover_snippet_for_platform "$platform")
    [ -z "$snippet" ] && return 0

    log_info "Configuring Moonraker deep-recovery shell_command..."

    # Skip the install entirely if Moonraker doesn't support shell_command on
    # this host. The frontend already falls back to firmware_restart in that
    # case, so we just don't pollute the conf with an unusable section.
    if moonraker_has_shell_command_component; then
        : # supported
    else
        local probe_status=$?
        if [ "$probe_status" = "1" ]; then
            log_warn "Moonraker on this host has no 'shell_command' component — skipping helix_recover install"
            log_warn "Frontend will fall back to FIRMWARE_RESTART for recovery"
            return 0
        fi
        # status 2 = couldn't reach moonraker (offline install, fresh image,
        # blocked port). Proceed and let moonraker warn-or-not at next start.
        log_info "Could not reach Moonraker at \${HELIX_MOONRAKER_URL:-http://127.0.0.1:7125}/server/info — installing snippet anyway"
    fi

    local conf
    conf=$(find_moonraker_conf)
    if [ -z "$conf" ]; then
        log_warn "Could not find moonraker.conf — skipping helix_recover install"
        log_warn "To enable Deep Recovery, manually add to your moonraker.conf:"
        echo ""
        printf '%s\n' "$snippet"
        echo ""
        return 0
    fi

    local fs
    fs=$(file_sudo "$conf")
    install_recovery_section "$conf" "$platform" "$fs"

    # Restart moonraker so the new shell_command is registered. Match the
    # pattern in configure_moonraker_updates(): only restart when we actually
    # changed the conf — install_recovery_section is idempotent and a no-op
    # on re-runs, but we can't tell from here, so always nudge moonraker.
    # Cheap on K2 (~3s); skipped during dry-runs by the caller.
    restart_moonraker 2>/dev/null || true
}

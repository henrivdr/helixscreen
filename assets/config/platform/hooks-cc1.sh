#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Elegoo Centauri Carbon 1 (Open Centauri firmware)
#
# The CC1 runs an OpenWrt/BusyBox system with procd init. The Open Centauri
# firmware manages Klipper + Moonraker. HelixScreen renders directly to the
# framebuffer (/dev/fb0, 480x272, 32bpp ARGB8888).
#
# SoC: Allwinner R528 (sun8iw20), Cortex-A7 dual-core, 128MB RAM
# Display: 480x272 LCD, Allwinner disp subsystem (no standard backlight iface)
# Touch: Goodix gt9xxnew_ts on /dev/input/event1
# Storage: UDISK (6.5GB ext4) mounted at /opt and /root
# SSH access: root@<ip> (port 22 or bind-shell on 4567)

# Boot-time self-heal of the COSMOS gui-switcher hijack.
#
# COSMOS replaces the squashfs rootfs via SWUpdate on upgrade but does NOT touch
# /data (which backs the /etc overlay where our sibling wrappers live). A future
# COSMOS migration could still clobber a wrapper in the overlay, restoring the
# stock grumpyscreen/guppyscreen/atomscreen init script. If that happens,
# gui-switcher would launch the stock UI instead of delegating to HelixScreen.
#
# This re-asserts every missing/clobbered wrapper on each boot. It is the runtime
# twin of the installer's _install_cc1_sibling_wrapper (competing_uis.sh) but is
# fully SELF-CONTAINED: hooks.sh is deployed standalone to ${INSTALL_DIR} and
# cannot source installer-lib files at runtime.
#
# Survivability: this hook lives at ${INSTALL_DIR}/platform/hooks.sh, deployed
# from /user-resource (ext4, not part of the squashfs rootfs SWUpdate replaces),
# and is sourced by /etc/init.d/helixscreen on every start. So the self-heal code
# itself survives upgrades, and runs before the UI launches.
#
# Idempotent and conservative: only acts on a sibling when the live init script
# LACKS the HELIXSCREEN_WRAPPER marker AND a .helix-bak backup already exists
# (i.e. the installer wrapped it once, then something restored the stock file).
# It never creates a fresh backup here — initial backup/wrap is the installer's
# job; this only repairs an existing hijack. CC1-guarded by /usr/bin/update-cosmos.
helix_platform_reassert_wrappers() {
    # CC1/COSMOS only — presence of the COSMOS updater is the device fingerprint.
    [ -x /usr/bin/update-cosmos ] || return 0

    local s target backup
    for s in grumpyscreen guppyscreen atomscreen; do
        target="/etc/init.d/${s}"
        backup="/etc/init.d/${s}.helix-bak"

        # Only repair siblings the installer already hijacked (backup present).
        [ -e "$backup" ] || continue
        # Already wrapped — nothing to repair.
        if grep -q "HELIXSCREEN_WRAPPER" "$target" 2>/dev/null; then
            continue
        fi

        echo "Re-asserting HelixScreen wrapper for /etc/init.d/${s} (clobbered by upgrade)"
        cat > "$target" <<'WRAPPER_EOF'
#!/bin/sh
# HELIXSCREEN_WRAPPER (do not remove this marker — used for idempotency)
# Re-asserted at boot by hooks-cc1.sh after a COSMOS upgrade clobbered the
# overlay copy. Delegates gui-switcher's launch to HelixScreen's init script.
# Original UI preserved at <name>.helix-bak and restored by the uninstaller.
exec /etc/init.d/helixscreen "$@"
WRAPPER_EOF
        chmod +x "$target" 2>/dev/null || true
    done
}

# Stop any competing screen UIs so HelixScreen has exclusive framebuffer access.
platform_stop_competing_uis() {
    # Stop any known competing third-party UIs
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen featherscreen FeatherScreen; do
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Kill python-based KlipperScreen if running
    # shellcheck disable=SC2009
    for pid in $(ps aux 2>/dev/null | grep -E 'python.*screen\.py' | grep -v grep | awk '{print $2}'); do
        echo "Killing KlipperScreen python process (PID $pid)"
        kill "$pid" 2>/dev/null || true
    done

    # Brief pause to let processes exit
    sleep 1
}

# The CC1 uses the Allwinner disp subsystem for backlight control.
# There is no standard /sys/class/backlight interface.
platform_enable_backlight() {
    return 0
}

# Open Centauri manages Klipper/Moonraker - they should be available by the
# time HelixScreen starts.
platform_wait_for_services() {
    return 0
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/opt/helixscreen/cache"

    # Repair any sibling gui-switcher wrapper a COSMOS upgrade clobbered in the
    # /etc overlay, so the next boot still hands the framebuffer to HelixScreen.
    # Runs before the UI launches; idempotent and CC1-guarded internally.
    helix_platform_reassert_wrappers

    # Logging policy: write to flash (/user-resource is ext4 with ~4 GB free),
    # NOT to /tmp. /tmp here is tmpfs backed by ~56 MiB shared RAM, and CC1
    # memory is tight — even moderate log volume there steals RAM from the UI.
    # /board-resource is also flash but small (~100 MiB, shared with Klipper);
    # /user-resource is the right place for app logs on this device.
    #
    # Rotation is constrained tighter than the spdlog default (5 MiB × 3 =
    # ~15 MiB) because helix logs are tiny at WARN/INFO and we don't want
    # surprise growth on flash. 1 MiB × 3 = ~3 MiB cap gives months of
    # headroom at normal levels.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/user-resource/helixscreen/logs/helix.log"
    export HELIX_LOG_ROTATE_BYTES=1048576
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/user-resource/helixscreen/logs" 2>/dev/null || true

    return 0
}

platform_post_stop() {
    return 0
}

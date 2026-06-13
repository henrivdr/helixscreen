#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Snapmaker U1 (Paxx Extended Firmware)
#
# The Snapmaker U1 runs Buildroot 2024.02 (Rockchip RK3562) with busybox SysV
# init — NOT Debian/systemd. Its stock touchscreen UI is /usr/bin/gui.
# HelixScreen uses the DRM backend for double-buffered page flipping.
#
# The build ships NO helix-watchdog, and busybox SysV init does not respawn S99
# children — so a boot-time SIGTERM to helix-screen would be permanent without a
# supervisor. We opt into the launcher's boot-time respawn self-heal below, and
# decouple WiFi bring-up from helix-screen's lifetime so a momentary UI death
# never strands the device off-network.
#
# DRM CRTC keepalive: The U1's display is driven by a Rockchip DRM/KMS
# pipeline (rockchipdrmfb). When the stock UI process exits, the kernel's
# VOP2 driver disables the CRTC, leaving the display permanently black.
# To prevent this, we spawn a background process that holds /dev/dri/card0
# open while we kill the stock UI. Once HelixScreen opens the DRM device
# itself, the keepalive process exits — but the CRTC stays active because
# HelixScreen now has its own fd on the device.
#
# Stock UI: /usr/bin/gui (started by S99screen init script)
# Camera supervisor: /usr/bin/lmd (started by /etc/init.d/S99v4l2-mpp-mipi at
#   boot — and *only* at boot; no fake-service auto-restart). lmd forks
#   /usr/bin/unisrv which subscribes to MQTT topic camera/request and answers
#   Klipper's TIMELAPSE_START / camera.take_a_photo RPCs.
# Touch: TLSC6x capacitive controller (tlsc6x_touch on /dev/input/event0)
# Display: 480x320 32bpp rockchipdrmfb (/dev/fb0)
# SSH access: root@<ip> (password: snapmaker) via extended firmware

# PID of the background keepalive process
DRM_KEEPALIVE_PID=""

# Opt into the launcher's boot-time respawn self-heal. The U1 ships no
# helix-watchdog and busybox init does not respawn S99 children, so a single
# boot-time SIGTERM to helix-screen (handled as _exit(0)) would otherwise be
# permanent. The launcher (helix-launcher.sh) reads HELIX_BOOT_RESPAWN_MAX and
# respawns a fast-exiting helix-screen up to this many times. Respect any value
# the user pinned in helixscreen.env.
export HELIX_BOOT_RESPAWN_MAX="${HELIX_BOOT_RESPAWN_MAX:-3}"

# WiFi restore configuration. The stock Snapmaker app saves the user's network
# to HELIX_SAVED_WPA and loads it into wpa_supplicant at runtime; since we
# replaced the stock app, we do it ourselves (see ensure_wifi_associated).
# Overridable for tests via the environment.
HELIX_SAVED_WPA="${HELIX_SAVED_WPA:-/oem/printer_data/gui/wpa_supplicant.conf}"
HELIX_WIFI_FLAG="${HELIX_WIFI_FLAG:-/tmp/helix-wifi-restore.active}"
HELIX_WIFI_IFACE="${HELIX_WIFI_IFACE:-wlan0}"

# Stop Snapmaker's stock touchscreen UI so HelixScreen can access the display.
#
# CRITICAL: We must hold /dev/dri/card0 open before killing the stock UI.
# Without this, the VOP2 driver disables the CRTC and the display goes
# permanently black until reboot.
platform_stop_competing_uis() {
    # Spawn a background process that holds the DRM device open.
    # It stays alive until helix-screen opens /dev/dri/card0 itself (detected
    # via /proc/*/fd), or for a maximum of 30 seconds as a safety timeout.
    if [ -e /dev/dri/card0 ]; then
        (
            # Hold the device open via our own fd
            exec 3>/dev/dri/card0
            echo "DRM keepalive: holding /dev/dri/card0 (pid $$)"
            # Wait until helix-screen has the device open, or timeout
            elapsed=0
            while [ "$elapsed" -lt 30 ]; do
                for pid_dir in /proc/[0-9]*/fd; do
                    pid=$(echo "$pid_dir" | sed 's|/proc/\([0-9]*\)/fd|\1|')
                    comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
                    case "$comm" in
                        helix-screen)
                            if readlink "$pid_dir"/* 2>/dev/null | grep -q '/dev/dri/card0'; then
                                echo "DRM keepalive: helix-screen (pid $pid) has /dev/dri/card0, releasing"
                                exit 0
                            fi
                            ;;
                    esac
                done
                sleep 1
                elapsed=$((elapsed + 1))
            done
            echo "DRM keepalive: timeout after 30s, releasing"
        ) &
        DRM_KEEPALIVE_PID=$!
        echo "DRM keepalive: background process PID $DRM_KEEPALIVE_PID"
    fi

    # Kill stock UI processes only. unisrv (proprietary camera/MQTT daemon
    # handling Klipper's TIMELAPSE_START) and lmd (its supervisor) are NOT UIs
    # and must keep running for timelapse to work. Killing lmd permanently
    # disables camera RPC until reboot — lmd has no supervisor of its own and
    # /etc/init.d/S99v4l2-mpp-mipi only starts it at boot.
    # NOTE: Do NOT call /etc/init.d/S99screen stop — on the U1, S99screen is
    # patched to delegate to helixscreen.init, which would cause infinite recursion.
    for ui in gui snapmaker-ui snapmaker-screen KlipperScreen klipperscreen; do
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

    # Brief pause to let processes settle
    sleep 1
}

# The U1 display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# The U1's SysV init starts Klipper/Moonraker (S60klipper/S61moonraker) before
# our S99 launcher, so they should be available by the time HelixScreen starts.
platform_wait_for_services() {
    return 0
}

# Ensure lmd (camera/MQTT supervisor) is running. lmd is started once at boot
# by /etc/init.d/S99v4l2-mpp-mipi with no auto-restart wrapper, so if it ever
# dies — historically because older helixscreen builds killed it in
# platform_stop_competing_uis — Klipper's TIMELAPSE_START hits a 5-second MQTT
# timeout and pauses the print. We re-launch lmd with the same imposter env
# the stock init uses; lmd then forks unisrv as its child.
ensure_lmd_running() {
    if pidof lmd >/dev/null 2>&1; then
        return 0
    fi
    if [ ! -x /usr/bin/lmd ]; then
        return 0
    fi
    if [ ! -e /tmp/capture-mipi-raw.sock ]; then
        # Capture pipeline not up yet; lmd would just fail to attach. Let
        # S99v4l2-mpp-mipi finish its sequence on its own.
        return 0
    fi
    echo "lmd not running — restarting camera supervisor"
    rm -f /var/run/unisrv.pid
    (
        export LD_PRELOAD=/usr/local/lib/libv4l2-imposter.so
        export V4L2_IMPOSTER_SOCKET_PATH=/tmp/capture-mipi-raw.sock
        export V4L2_IMPOSTER_DEVICE=/dev/video11
        export V4L2_IMPOSTER_WIDTH=1920
        export V4L2_IMPOSTER_HEIGHT=1080
        export V4L2_IMPOSTER_FORMAT=nv12
        start-stop-daemon -S -b -q -m -p /var/run/unisrv.pid -x /usr/bin/lmd
    )
}

# True (0) if the WiFi interface reports an associated/completed state.
_helix_wifi_connected() {
    wpa_cli -i "$HELIX_WIFI_IFACE" status 2>/dev/null | grep -q '^wpa_state=COMPLETED'
}

# Synchronously push the saved network into wpa_supplicant. Idempotent and safe
# to call repeatedly: returns 0 if it applied (or the link was already up), 1 if
# there was nothing to apply (no saved config, no wpa_cli, or empty credentials).
_helix_wifi_apply_saved() {
    [ -f "$HELIX_SAVED_WPA" ] || return 1
    command -v wpa_cli >/dev/null 2>&1 || return 1

    _ssid=$(grep 'ssid=' "$HELIX_SAVED_WPA" | head -1 | sed 's/.*ssid="\(.*\)"/\1/')
    _psk=$(grep 'psk=' "$HELIX_SAVED_WPA" | head -1 | sed 's/.*psk="\(.*\)"/\1/')
    [ -n "$_ssid" ] && [ -n "$_psk" ] || return 1

    # Already connected — don't churn an up link.
    if _helix_wifi_connected; then
        return 0
    fi

    _netid=$(wpa_cli -i "$HELIX_WIFI_IFACE" add_network 2>/dev/null | tail -1)
    if [ -n "$_netid" ] && [ "$_netid" != "FAIL" ]; then
        wpa_cli -i "$HELIX_WIFI_IFACE" set_network "$_netid" ssid "\"$_ssid\"" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" set_network "$_netid" psk "\"$_psk\"" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" enable_network "$_netid" >/dev/null 2>&1
        wpa_cli -i "$HELIX_WIFI_IFACE" select_network "$_netid" >/dev/null 2>&1
        echo "WiFi: restoring saved network '$_ssid' (id=$_netid)"
    fi
    return 0
}

# Bring WiFi up INDEPENDENTLY of helix-screen's lifetime.
#
# The stock Snapmaker UI (which we replace) is what loads the user's saved WiFi
# credentials into wpa_supplicant at runtime. With the stock UI disabled, only
# we restore WiFi — and the old approach (one synchronous attempt in
# platform_pre_start) both raced wpa_supplicant/wlan0 at early boot AND coupled
# network recovery to helix's start path, so a momentary helix death stranded
# the device off-network and unreachable.
#
# So we spawn a DETACHED, idempotent worker that waits for the interface, then
# applies the saved network and retries until it associates. The worker outlives
# helix-screen (a `( ) &` grandchild reparented to init; the launcher's cleanup
# trap only killalls helix-* by name, not this sh worker), so the device stays
# reachable even if the UI dies. Returns immediately — never blocks boot.
ensure_wifi_associated() {
    [ -f "$HELIX_SAVED_WPA" ] || return 0
    command -v wpa_cli >/dev/null 2>&1 || return 0

    # Already up — nothing to do.
    if _helix_wifi_connected; then
        return 0
    fi

    # Single-flight: don't stack workers across the init + launcher pre_start
    # calls (both run platform_pre_start). A live worker owns HELIX_WIFI_FLAG.
    if [ -f "$HELIX_WIFI_FLAG" ] && kill -0 "$(cat "$HELIX_WIFI_FLAG" 2>/dev/null)" 2>/dev/null; then
        return 0
    fi

    (
        trap 'rm -f "$HELIX_WIFI_FLAG" 2>/dev/null || true' EXIT
        # Wait for wpa_supplicant's control interface (up to ~30s).
        _w=0
        while [ "$_w" -lt 30 ]; do
            wpa_cli -i "$HELIX_WIFI_IFACE" status >/dev/null 2>&1 && break
            sleep 1
            _w=$((_w + 1))
        done
        # Apply + verify, a few attempts.
        _try=0
        while [ "$_try" -lt 6 ]; do
            _helix_wifi_connected && break
            _helix_wifi_apply_saved
            sleep 3
            _try=$((_try + 1))
        done
    ) &
    echo $! > "$HELIX_WIFI_FLAG" 2>/dev/null || true
    return 0
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/userdata/helixscreen/cache"
    # Force DRM device — skip auto-detection which may race with connector state
    export HELIX_DRM_DEVICE="/dev/dri/card0"

    # Recover the camera supervisor if a prior helixscreen build killed it.
    # Idempotent: no-op when lmd is already alive.
    ensure_lmd_running

    # Bring WiFi up independently of helix-screen's lifetime (detached worker).
    # Must NOT block boot or strand the device if helix later dies.
    ensure_wifi_associated

    return 0
}

platform_post_stop() {
    # Kill the keepalive process if still running
    if [ -n "$DRM_KEEPALIVE_PID" ]; then
        kill "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        wait "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        echo "DRM keepalive: cleaned up process $DRM_KEEPALIVE_PID"
        DRM_KEEPALIVE_PID=""
    fi

    # Do NOT restart /usr/bin/gui — the stock Snapmaker UI takes ownership of
    # wpa_supplicant on launch and drops the active WiFi connection, breaking
    # SSH and any in-flight install/update mid-stream (issue #797). Leaving the
    # display blank during stop is preferable to wedging the network. The stock
    # UI is restored by the uninstaller when the user explicitly removes us.
    return 0
}

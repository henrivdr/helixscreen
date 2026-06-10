#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Creality K2 / K2 Pro / K2 Plus
#
# K2 series runs OpenWrt 21.02 with procd init system.
# Stock UI is managed by /etc/init.d/app (procd service).
# Processes: display-server, web-server, Monitor, master-server, etc.

platform_stop_competing_uis() {
    # Stop the stock Creality UI via procd (clean shutdown)
    if [ -f /etc/init.d/app ]; then
        /etc/init.d/app stop 2>/dev/null || true
    fi

    # Kill any lingering stock UI processes
    for proc in display-server Monitor master-server audio-server \
                wifi-server app-server upgrade-server; do
        killall "$proc" 2>/dev/null || true
    done

    # Note: web-server is intentionally NOT killed — it serves the
    # Creality Cloud integration and camera stream (webrtc_local).
    # Stopping it would break remote monitoring via Creality app.

    # Persistently disable the stock UI service (reversible)
    if [ -x /etc/init.d/app ]; then
        /etc/init.d/app disable 2>/dev/null || true
    fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    # Wait for Moonraker to be ready (K2 Moonraker is on port 7125).
    # Track elapsed seconds via `date +%s` so the timeout reflects actual wall
    # time, with a 1s urlopen timeout for quick retries. K2's dual Cortex-A7 is
    # slow; Moonraker+Klipper cold-boot init routinely exceeds 30s, so allow up
    # to 120s (matching hooks-ad5m-forgex) and log progress every ~10s so the
    # launcher log doesn't look hung. On timeout the UI launches anyway —
    # MoonrakerClient auto-reconnects indefinitely (~2s backoff), so a late
    # Moonraker just shows a brief disconnected state then connects.
    # Use 127.0.0.1 (not localhost): K2 has no /etc/hosts entry for localhost.
    local timeout_sec=120
    local start_sec elapsed
    start_sec=$(date +%s)
    while :; do
        elapsed=$(( $(date +%s) - start_sec ))
        [ "$elapsed" -ge "$timeout_sec" ] && break
        if python3 -c "
import urllib.request
try:
    urllib.request.urlopen('http://127.0.0.1:7125/server/info', timeout=1)
    exit(0)
except:
    exit(1)
" 2>/dev/null; then
            return 0
        fi
        [ $((elapsed % 10)) -eq 0 ] && echo "[hooks-k2] Waiting for Moonraker... (${elapsed}s)"
        sleep 1
    done
    echo "[hooks-k2] Warning: Moonraker not ready after ${timeout_sec}s (UI will start; it auto-reconnects)"
    return 1
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/mnt/UDISK/helixscreen/cache"

    # Logging policy: write to /mnt/UDISK (27 GB UDISK partition, ext4).
    # K2 /tmp is a 244 MB tmpfs (more headroom than CC1/AD5M) and /var/log
    # is on the persistent overlayfs root, so the init-script awk resolver
    # already keeps launcher.log out of RAM. But routing the structured
    # spdlog stream to UDISK too keeps everything in one place and avoids
    # filling the 240 MB overlay with debug volume when someone leaves
    # HELIX_LOG_LEVEL=debug enabled in helixscreen.env.
    export HELIX_LOG_DEST=file
    export HELIX_LOG_FILE="/mnt/UDISK/helixscreen/logs/helix.log"
    export HELIX_LOG_ROTATE_BYTES=2097152
    export HELIX_LOG_ROTATE_FILES=3
    mkdir -p "/mnt/UDISK/helixscreen/logs" 2>/dev/null || true

    # K2 has no curl — ensure HelixScreen knows to skip HTTPS features
    # SSL is disabled in the K2 build, but set this for safety
    export HELIX_DISABLE_SSL=1

    # Bring up WiFi in the background. The stock wifi-server manages
    # wpa_supplicant but we killed it in platform_stop_competing_uis(),
    # so we re-establish it here as a courtesy. Running synchronously
    # cost ~7s (kill + sleep 1 + wpa_supplicant + sleep 3 + udhcpc) and
    # blocked the entire boot path even when eth0 was already up — pure
    # waste. Backgrounding lets boot proceed; helix-screen reaches
    # Moonraker over localhost regardless, and external connectivity
    # (updates, telemetry) follows whichever interface ends up associated.
    if [ -e /sys/class/net/wlan0 ] && ! ip addr show wlan0 2>/dev/null | grep -q 'inet '; then
        local wpa_conf="/etc/wifi/wpa_supplicant/wpa_supplicant.conf"
        if [ -f "$wpa_conf" ]; then
            (
                killall wpa_supplicant 2>/dev/null || true
                sleep 1
                wpa_supplicant -B -i wlan0 -c "$wpa_conf" -D nl80211 2>/dev/null || true
                sleep 3
                udhcpc -i wlan0 -n -q 2>/dev/null || true
            ) >/dev/null 2>&1 &
        fi
    fi
}

platform_post_stop() {
    # Re-enable stock UI if HelixScreen is being uninstalled
    # (installer calls this; normal stop does NOT re-enable)
    :
}

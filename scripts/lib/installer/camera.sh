#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: camera
# K2 ustreamer camera setup: replace the stock proprietary WebRTC pipeline
# (which HelixScreen and fluidd cannot consume) with a static ustreamer MJPEG
# server, point both UIs at it, and make uninstall fully restore stock state.
#
# Reuses the framework's reversal machinery: the stock webrtc init scripts are
# disabled via record_disabled_service "sysv-chmod" so the existing
# reenable_disabled_services() (uninstall.sh) chmod +x's them back on uninstall.
# cam_app is hotplug-launched with no watchdog, so killing it is clean and it
# returns on reboot once webrtc is re-enabled.
#
# Reads: INSTALL_DIR, SUDO, _PY_BIN (via _has_python from common.sh),
#        record_disabled_service (competing_uis.sh)
# Writes: /etc/init.d/ustreamer, ${INSTALL_DIR}/config/.disabled_services,
#         ${INSTALL_DIR}/config/.webcams_backup.json,
#         ${INSTALL_DIR}/config/.camera_migrated (marker), moonraker webcams

# Source guard
[ -n "${_HELIX_CAMERA_SOURCED:-}" ] && return 0
_HELIX_CAMERA_SOURCED=1

# Moonraker base URL (host:port). Overridable for tests.
: "${MOONRAKER_URL:=http://127.0.0.1:7125}"

# init.d / rc.d directories. Resolved per-call (not at source time) so the BATS
# suite can redirect them away from the host. Production callers leave the
# HELIX_INITD_DIR / HELIX_RCD_DIR env vars unset.
_initd_dir() { echo "${HELIX_INITD_DIR:-/etc/init.d}"; }
_rcd_dir()   { echo "${HELIX_RCD_DIR:-/etc/rc.d}"; }

# Marker recording that we migrated moonraker's webcam list (so uninstall knows
# to restore from the backup). Lives in INSTALL_DIR/config alongside the backup.
_camera_marker_file()  { echo "${INSTALL_DIR}/config/.camera_migrated"; }
_camera_backup_file()  { echo "${INSTALL_DIR}/config/.webcams_backup.json"; }

# Our webcam entry name in moonraker.
HELIX_WEBCAM_NAME="HelixScreen Camera"

# Detect the K2's primary LAN IPv4 address (busybox-compatible).
# Strategy:
#   1. `ip route get 1.1.1.1` and pull the "src <addr>" field (the address the
#      kernel would use to reach the internet — i.e. the primary LAN IP).
#   2. Fall back to the default-route interface's first inet address.
#   3. Fall back to the first non-loopback inet address from `ip addr`.
# Echoes the address, or empty if none found.
detect_lan_ip() {
    local ip_addr=""

    if command -v ip >/dev/null 2>&1; then
        # 1. src field from a route lookup toward a public address.
        ip_addr=$(ip route get 1.1.1.1 2>/dev/null | \
            awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i+1); exit } }')

        # 2. Default-route interface's first inet address.
        if [ -z "$ip_addr" ]; then
            local def_iface
            def_iface=$(ip route 2>/dev/null | awk '/^default/ { print $5; exit }')
            if [ -n "$def_iface" ]; then
                ip_addr=$(ip -4 addr show "$def_iface" 2>/dev/null | \
                    awk '/inet / { sub(/\/.*/, "", $2); print $2; exit }')
            fi
        fi

        # 3. First non-loopback inet address anywhere.
        if [ -z "$ip_addr" ]; then
            ip_addr=$(ip -4 addr show 2>/dev/null | \
                awk '/inet / && $2 !~ /^127\./ { sub(/\/.*/, "", $2); print $2; exit }')
        fi
    fi

    # Last resort: ifconfig (busybox) for boxes without iproute2.
    if [ -z "$ip_addr" ] && command -v ifconfig >/dev/null 2>&1; then
        ip_addr=$(ifconfig 2>/dev/null | \
            awk '/inet (addr:)?[0-9]/ {
                for (i = 1; i <= NF; i++) {
                    a = $i; sub(/^addr:/, "", a)
                    if (a ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ && a !~ /^127\./) { print a; exit }
                }
            }')
    fi

    echo "$ip_addr"
}

# Query moonraker's webcam list, return the raw JSON "webcams" array (the value
# of result.webcams) on stdout, or empty on any failure. Uses python3/urllib
# (K2 has no curl/wget). Caller is responsible for parsing.
_moonraker_get_webcams_json() {
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request
base = sys.argv[1].rstrip('/')
try:
    with urllib.request.urlopen(base + '/server/webcams/list', timeout=5) as r:
        data = json.load(r)
    cams = data.get('result', {}).get('webcams', [])
    sys.stdout.write(json.dumps(cams))
except Exception:
    sys.exit(1)
PYEOF
}

# Returns 0 if moonraker already has a webcam exposing a usable MJPEG/ustreamer
# stream (service name contains mjpeg/ustreamer AND a non-empty stream_url).
# Used to bail out without stomping a working camera setup.
_moonraker_has_usable_mjpeg() {
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request
base = sys.argv[1].rstrip('/')
try:
    with urllib.request.urlopen(base + '/server/webcams/list', timeout=5) as r:
        data = json.load(r)
except Exception:
    sys.exit(2)  # unreachable -> not "has usable" (distinct from found)
cams = data.get('result', {}).get('webcams', [])
for c in cams:
    svc = (c.get('service') or '').lower()
    url = c.get('stream_url') or ''
    if ('mjpeg' in svc or 'ustreamer' in svc) and url.strip():
        sys.exit(0)   # found a usable one
sys.exit(1)           # none found
PYEOF
}

# Probe whether ustreamer is listening on the given port (busybox-compatible).
# Tries python3 socket connect first; falls back to the /snapshot endpoint.
# Args: $1 = port. Returns 0 if reachable.
_ustreamer_listening() {
    local port="$1"
    if _has_python; then
        "$_PY_BIN" - "$port" <<'PYEOF' 2>/dev/null && return 0
import socket, sys
port = int(sys.argv[1])
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
try:
    s.connect(('127.0.0.1', port))
    sys.exit(0)
except Exception:
    sys.exit(1)
finally:
    s.close()
PYEOF
    fi
    return 1
}

# Back up moonraker's current webcam list to the backup file (for restore on
# uninstall) and drop the migration marker. Idempotent: never overwrites an
# existing backup (the first backup is the true pre-HelixScreen state; a second
# install pass must not archive our own migrated list as if it were stock).
# Args: $1 = webcams JSON array
_record_webcam_backup() {
    local cams_json="$1"
    local backup marker
    backup="$(_camera_backup_file)"
    marker="$(_camera_marker_file)"

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    if [ ! -f "$backup" ]; then
        printf '%s' "$cams_json" | $(file_sudo "${INSTALL_DIR}/config") tee "$backup" >/dev/null
    fi
    # Marker is harmless to (re)touch; presence is what uninstall checks.
    $(file_sudo "${INSTALL_DIR}/config") touch "$marker" 2>/dev/null || \
        : > "$marker" 2>/dev/null || true
}

# Delete the stock iframe "Default" webcam (if present) and POST our ustreamer
# webcam pointed at http://<lan_ip>:<port>/. Idempotent on the moonraker side:
# POST /server/webcams/item upserts by name, so re-running just refreshes it.
# Args: $1 = lan_ip, $2 = port. Returns non-zero only on hard python failure.
_moonraker_migrate_webcams() {
    local lan_ip="$1" port="$2"
    _has_python || return 1
    "$_PY_BIN" - "$MOONRAKER_URL" "$lan_ip" "$port" "$HELIX_WEBCAM_NAME" <<'PYEOF' 2>/dev/null
import json, sys, urllib.request, urllib.parse
base, lan_ip, port, name = sys.argv[1].rstrip('/'), sys.argv[2], sys.argv[3], sys.argv[4]

def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        r.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(r, timeout=5) as resp:
        return json.load(resp)

# Drop the stock iframe/webrtc "Default" webcam if it exists (it points at the
# proprietary WebRTC iframe HelixScreen/fluidd can't render). Only DATABASE-
# sourced entries can be removed via the API; a `source: config` webcam (e.g.
# the DnG K2-Camera hack defines one in moonraker.conf) is read-only here — we
# leave it and print CONFIG_DEFAULT_LEFT so the shell can warn the user.
try:
    cams = req('GET', '/server/webcams/list').get('result', {}).get('webcams', [])
    for c in cams:
        nm = c.get('name', '')
        svc = (c.get('service') or '').lower()
        src = (c.get('source') or '').lower()
        if nm == 'Default' and ('iframe' in svc or 'webrtc' in svc):
            if src == 'config':
                sys.stdout.write('CONFIG_DEFAULT_LEFT\n')
            else:
                req('DELETE', '/server/webcams/item?name=' + urllib.parse.quote(nm))
except Exception:
    pass  # non-fatal; the POST below is what matters

stream = 'http://%s:%s/stream' % (lan_ip, port)
snap = 'http://%s:%s/snapshot' % (lan_ip, port)
try:
    # 'mjpegstreamer-adaptive' (not 'ustreamer'): fluidd/mainsail render MJPEG by
    # service type and have no 'ustreamer' renderer — it shows "service not
    # supported!" and never displays frames. ustreamer's /stream + /snapshot are
    # the standard mjpegstreamer endpoints, so 'mjpegstreamer-adaptive' renders
    # correctly in both web UIs and still matches HelixScreen's own is_mjpeg
    # consumer check (which keys on the 'mjpeg' substring).
    req('POST', '/server/webcams/item', {
        'name': name,
        'service': 'mjpegstreamer-adaptive',
        'stream_url': stream,
        'snapshot_url': snap,
        'enabled': True,
        'target_fps': 15,
    })
except Exception:
    sys.exit(1)
sys.exit(0)
PYEOF
}

# Install the ustreamer camera setup on K2. No-op on any other platform.
# Idempotent and reversal-aware. See module header.
# Args: $1 = platform
install_camera_k2() {
    local platform="${1:-}"
    [ "$platform" = "k2" ] || return 0

    log_info "Configuring K2 ustreamer camera..."

    # (a) DETECT: don't stomp an already-working MJPEG/ustreamer camera.
    if _moonraker_has_usable_mjpeg; then
        log_info "Moonraker already exposes a usable MJPEG camera — leaving it untouched"
        return 0
    fi

    # (b) Verify the bundled ustreamer binary is present + executable.
    local ustreamer_bin="${INSTALL_DIR}/bin/ustreamer"
    if [ ! -x "$ustreamer_bin" ]; then
        log_warn "ustreamer binary missing or not executable at $ustreamer_bin"
        log_warn "Skipping K2 camera setup (release bundle may be incomplete)"
        return 0
    fi

    # (c) Free the device: disable + stop the stock WebRTC pipeline so ustreamer
    # can claim exclusive /dev/video0. chmod a-x mirrors the competing_uis
    # pattern and is recorded as sysv-chmod so uninstall's
    # reenable_disabled_services() chmod +x's them back. cam_app is
    # hotplug-launched (no watchdog) so killing it is clean.
    local initd script
    initd="$(_initd_dir)"
    for script in "${initd}/webrtc" "${initd}/webrtc_local"; do
        if [ -f "$script" ]; then
            log_info "Disabling stock WebRTC init script: $script"
            $SUDO "$script" stop 2>/dev/null || true
            $SUDO chmod a-x "$script" 2>/dev/null || true
            record_disabled_service "sysv-chmod" "$script"
        fi
    done
    # Release the device held by the stock capture apps (ignore errors).
    killall cam_app cam_sub_app 2>/dev/null || true

    # (d) Install + enable the procd ustreamer service (idempotent).
    local svc_dest="${initd}/ustreamer"
    local svc_src="${INSTALL_DIR}/config/helixscreen-ustreamer-k2.sh"
    if [ ! -f "$svc_src" ]; then
        log_warn "ustreamer init source missing: $svc_src — skipping camera service install"
        return 0
    fi

    if [ -f "$svc_dest" ]; then
        log_info "ustreamer init script already installed at $svc_dest"
    else
        log_info "Installing ustreamer procd init script..."
        $SUDO cp "$svc_src" "$svc_dest" 2>/dev/null || \
            log_warn "Could not install ustreamer init at $svc_dest"
        $SUDO chmod +x "$svc_dest" 2>/dev/null || true
        $SUDO "$svc_dest" enable 2>/dev/null || \
            log_warn "ustreamer enable failed — camera may not autostart at boot"
    fi

    # Always (re)start; restart is safe if already running.
    if [ -x "$svc_dest" ]; then
        $SUDO "$svc_dest" restart 2>/dev/null || $SUDO "$svc_dest" start 2>/dev/null || true
    fi

    # Verify it's actually listening before we point moonraker at it. The retry
    # count is overridable (HELIX_USTREAMER_PROBE_TRIES) so tests can skip the
    # multi-second wait when no real ustreamer is running.
    local port="8080"
    local tries="${HELIX_USTREAMER_PROBE_TRIES:-5}"
    local i=0 listening=false
    while [ "$i" -lt "$tries" ]; do
        if _ustreamer_listening "$port"; then
            listening=true
            break
        fi
        i=$((i + 1))
        [ "$i" -lt "$tries" ] && sleep 1
    done
    if [ "$listening" = true ]; then
        log_success "ustreamer is serving MJPEG on :$port"
    else
        log_warn "ustreamer does not appear to be listening on :$port (check /dev/video0)"
    fi

    # (e) Moonraker webcam migration (preserve/fix fluidd). Back up the current
    # list, then delete the stock iframe and add our ustreamer webcam. If
    # moonraker is unreachable, warn but leave ustreamer running (the camera
    # works once moonraker comes up — don't fail the whole install).
    # Guard the assignment: _moonraker_get_webcams_json exits non-zero when
    # Moonraker is unreachable, which would abort under the installer's `set -e`.
    local cams_json=""
    cams_json="$(_moonraker_get_webcams_json)" || cams_json=""
    if [ -z "$cams_json" ]; then
        log_warn "Moonraker unreachable at $MOONRAKER_URL — ustreamer is running, but"
        log_warn "the webcam was not registered. It will work once Moonraker is up;"
        log_warn "re-run the installer or add the webcam manually if needed."
        return 0
    fi

    local lan_ip
    lan_ip="$(detect_lan_ip)"
    if [ -z "$lan_ip" ]; then
        log_warn "Could not determine the K2's LAN IP — skipping webcam registration"
        log_warn "ustreamer is running on :$port; add the webcam manually if needed."
        return 0
    fi

    # Record the pre-migration list ONCE (true stock state) for reversal.
    _record_webcam_backup "$cams_json"

    log_info "Registering ustreamer webcam (http://${lan_ip}:${port}/) in Moonraker..."
    local migrate_out
    if migrate_out="$(_moonraker_migrate_webcams "$lan_ip" "$port")"; then
        log_success "Moonraker webcam configured for HelixScreen + fluidd"
        case "$migrate_out" in
            *CONFIG_DEFAULT_LEFT*)
                log_warn "A stock 'Default' webcam is defined in your Moonraker *config* (e.g. the K2-Camera hack) — it can't be removed via the API."
                log_warn "  HelixScreen uses the MJPEG stream regardless; fluidd will list both until you remove that config entry."
                ;;
        esac
    else
        log_warn "Failed to register the ustreamer webcam in Moonraker"
        log_warn "ustreamer is running on :$port; add it manually in fluidd if needed."
    fi
}

# Reverse install_camera_k2. No-op on any other platform. Idempotent.
# NOTE: re-enabling the stock webrtc init scripts is handled by the framework's
# reenable_disabled_services() (chmod +x on the recorded sysv-chmod entries);
# we deliberately do NOT duplicate that here. cam_app returns on reboot.
# Args: $1 = platform
uninstall_camera_k2() {
    local platform="${1:-}"
    [ "$platform" = "k2" ] || return 0

    log_info "Removing K2 ustreamer camera..."

    # (a) Stop, disable, and remove the ustreamer service + binary.
    local initd rcd svc_dest
    initd="$(_initd_dir)"
    rcd="$(_rcd_dir)"
    svc_dest="${initd}/ustreamer"
    if [ -f "$svc_dest" ]; then
        $SUDO "$svc_dest" stop 2>/dev/null || true
        $SUDO "$svc_dest" disable 2>/dev/null || true
        $SUDO rm -f "$svc_dest"
        # Belt-and-suspenders: drop any procd rc.d symlinks it created.
        $SUDO rm -f "${rcd}/S95ustreamer" "${rcd}/K05ustreamer" 2>/dev/null || true
    fi
    killall ustreamer 2>/dev/null || true
    $SUDO rm -f "${INSTALL_DIR}/bin/ustreamer" 2>/dev/null || true

    # (b) Restore moonraker webcams from the backup, if we migrated them.
    local marker backup
    marker="$(_camera_marker_file)"
    backup="$(_camera_backup_file)"
    if [ -f "$marker" ]; then
        if [ -f "$backup" ] && _has_python; then
            log_info "Restoring Moonraker webcam list from backup..."
            "$_PY_BIN" - "$MOONRAKER_URL" "$backup" "$HELIX_WEBCAM_NAME" <<'PYEOF' 2>/dev/null || \
                log_warn "Could not fully restore Moonraker webcams (Moonraker may be down)"
import json, sys, urllib.request, urllib.parse
base, backup_path, name = sys.argv[1].rstrip('/'), sys.argv[2], sys.argv[3]

def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(base + path, data=data, method=method)
    if data is not None:
        r.add_header('Content-Type', 'application/json')
    with urllib.request.urlopen(r, timeout=5) as resp:
        return json.load(resp)

# Remove our entry first.
try:
    req('DELETE', '/server/webcams/item?name=' + urllib.parse.quote(name))
except Exception:
    pass

# Re-POST every backed-up (stock) webcam entry.
try:
    with open(backup_path) as f:
        cams = json.load(f)
except Exception:
    cams = []

for c in cams:
    if not c.get('name'):
        continue
    body = {
        'name': c.get('name'),
        'service': c.get('service', 'mjpegstreamer'),
        'enabled': c.get('enabled', True),
    }
    for k in ('stream_url', 'snapshot_url', 'target_fps', 'flip_horizontal',
              'flip_vertical', 'rotation', 'aspect_ratio', 'icon', 'location'):
        if c.get(k) is not None:
            body[k] = c[k]
    try:
        req('POST', '/server/webcams/item', body)
    except Exception:
        pass
sys.exit(0)
PYEOF
        else
            log_warn "Webcam backup missing or python unavailable — only removed our entry"
        fi
        $(file_sudo "$backup") rm -f "$backup" 2>/dev/null || true
        $(file_sudo "$marker") rm -f "$marker" 2>/dev/null || true
    fi

    log_success "K2 ustreamer camera removed (stock WebRTC re-enabled on reboot)"
}

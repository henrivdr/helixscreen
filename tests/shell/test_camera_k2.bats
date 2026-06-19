#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the K2 ustreamer camera installer module (camera.sh).
# Covers: platform gating, the detect-first no-stomp guard, the procd init
# install, sysv-chmod disable recording of stock WebRTC, webcam backup, LAN-IP
# detection, idempotent re-install, and the uninstall teardown.
#
# Moonraker is mocked at the Python layer: $MOONRAKER_URL points at a local
# fake HTTP server (started per-test when needed). The stock WebRTC init scripts
# and ustreamer init are mocked as plain files in a temp /etc/init.d.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"
    export DISABLED_SERVICES_FILE="$INSTALL_DIR/config/.disabled_services"
    export WEBCAM_BACKUP="$INSTALL_DIR/config/.webcams_backup.json"
    export CAMERA_MARKER="$INSTALL_DIR/config/.camera_migrated"

    # Fake init.d / rc.d so the installer's chmod/stop/enable/cp hit temp files,
    # not the host. camera.sh honors these env overrides (resolved per-call).
    export HELIX_INITD_DIR="$BATS_TEST_TMPDIR/etc/init.d"
    export HELIX_RCD_DIR="$BATS_TEST_TMPDIR/etc/rc.d"
    export FAKE_INITD="$HELIX_INITD_DIR"
    mkdir -p "$HELIX_INITD_DIR" "$HELIX_RCD_DIR"

    SUDO=""
    export SUDO

    HELIX_INSTALL_DIRS="$INSTALL_DIR"

    # Default: Moonraker unreachable (port nobody listens on) so install can run
    # the device/service path without needing a live server. Tests that exercise
    # webcam migration override this with start_fake_moonraker.
    export MOONRAKER_URL="http://127.0.0.1:1"

    # Skip the multi-second ustreamer listen wait (no real daemon in tests).
    export HELIX_USTREAMER_PROBE_TRIES=1

    # Ship the real procd init source into INSTALL_DIR/config so the install
    # path exercises the actual cp (as it would on-device from the bundle).
    cp "$WORKTREE_ROOT/config/helixscreen-ustreamer-k2.sh" \
        "$INSTALL_DIR/config/helixscreen-ustreamer-k2.sh"

    # Reset source guards and source the module + the framework bits it needs.
    unset _HELIX_COMMON_SOURCED _HELIX_COMPETING_UIS_SOURCED _HELIX_CAMERA_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/competing_uis.sh"  # record_disabled_service
    . "$WORKTREE_ROOT/scripts/lib/installer/camera.sh"

    # Stub killall (not present / would hit host processes).
    killall() { :; }
    export -f killall

    # Point the module's init paths at our fakes by overriding the constants and
    # using a tiny init-dispatch shim. We can't easily redirect /etc/init.d, so
    # we override the helper functions that touch it via wrappers below.
}

# Create a fake stock WebRTC init script that records its invocations.
make_fake_webrtc() {
    local name="$1"
    local path="$FAKE_INITD/$name"
    cat > "$path" <<EOF
#!/bin/sh
echo "\$1" >> "$BATS_TEST_TMPDIR/${name}.calls"
exit 0
EOF
    chmod +x "$path"
    echo "$path"
}

# Start a fake Moonraker that serves a webcam list and records POST/DELETE.
# Writes the chosen port into MOONRAKER_URL. Body file at $BATS_TEST_TMPDIR/mr.
start_fake_moonraker() {
    local cams_json="$1"   # JSON array string for result.webcams
    local statefile="$BATS_TEST_TMPDIR/mr_state.json"
    printf '%s' "$cams_json" > "$statefile"
    export MR_REQUESTS="$BATS_TEST_TMPDIR/mr_requests.log"
    : > "$MR_REQUESTS"

    python3 - "$statefile" "$MR_REQUESTS" "$BATS_TEST_TMPDIR/mr_port" <<'PYEOF' &
import json, sys, http.server, threading
state_path, req_log, port_file = sys.argv[1], sys.argv[2], sys.argv[3]

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        with open(req_log, 'a') as f: f.write('GET %s\n' % self.path)
        if self.path.startswith('/server/webcams/list'):
            with open(state_path) as f: cams = json.load(f)
            return self._send({'result': {'webcams': cams}})
        self._send({'result': {}})
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n).decode() if n else ''
        with open(req_log, 'a') as f: f.write('POST %s %s\n' % (self.path, body))
        self._send({'result': {}})
    def do_DELETE(self):
        with open(req_log, 'a') as f: f.write('DELETE %s\n' % self.path)
        self._send({'result': {}})

srv = http.server.HTTPServer(('127.0.0.1', 0), H)
with open(port_file, 'w') as f: f.write(str(srv.server_address[1]))
srv.serve_forever()
PYEOF
    FAKE_MR_PID=$!
    export FAKE_MR_PID
    # Wait for the port file.
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -s "$BATS_TEST_TMPDIR/mr_port" ] && break
        sleep 0.2
    done
    export MOONRAKER_URL="http://127.0.0.1:$(cat "$BATS_TEST_TMPDIR/mr_port")"
}

teardown() {
    [ -n "${FAKE_MR_PID:-}" ] && kill "$FAKE_MR_PID" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Platform gating
# ---------------------------------------------------------------------------

@test "install_camera_k2: no-op on non-k2 platform" {
    run install_camera_k2 "pi"
    [ "$status" -eq 0 ]
    [ ! -f "$DISABLED_SERVICES_FILE" ]
    [ ! -f "$WEBCAM_BACKUP" ]
}

@test "uninstall_camera_k2: no-op on non-k2 platform" {
    run uninstall_camera_k2 "ad5m"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Detect-first no-stomp guard
# ---------------------------------------------------------------------------

@test "install: bails out without stomping when a usable MJPEG webcam exists" {
    start_fake_moonraker '[{"name":"Cam","service":"mjpegstreamer","stream_url":"http://x/stream"}]'
    # ustreamer binary present so we'd otherwise proceed.
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    # No disable recorded, no backup, no migration marker — nothing was touched.
    [ ! -f "$DISABLED_SERVICES_FILE" ]
    [ ! -f "$WEBCAM_BACKUP" ]
    [ ! -f "$CAMERA_MARKER" ]
}

# ---------------------------------------------------------------------------
# Missing binary
# ---------------------------------------------------------------------------

@test "install: warns and returns when ustreamer binary missing" {
    rm -f "$INSTALL_DIR/bin/ustreamer"
    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    [ ! -f "$DISABLED_SERVICES_FILE" ]
}

# ---------------------------------------------------------------------------
# State recording: disable stock WebRTC + record sysv-chmod (direct unit test of
# the recording contract via record_disabled_service, the framework primitive
# install_camera_k2 calls)
# ---------------------------------------------------------------------------

@test "install records sysv-chmod disables for both webrtc init scripts" {
    # Drive the device-freeing step directly against fake init scripts, matching
    # what install_camera_k2 does in step (c). This isolates the reversal-state
    # contract from /etc/init.d hardcoding.
    local w1 w2
    w1=$(make_fake_webrtc webrtc)
    w2=$(make_fake_webrtc webrtc_local)

    for script in "$w1" "$w2"; do
        $SUDO "$script" stop
        $SUDO chmod a-x "$script"
        record_disabled_service "sysv-chmod" "$script"
    done

    grep -q "sysv-chmod:$w1" "$DISABLED_SERVICES_FILE"
    grep -q "sysv-chmod:$w2" "$DISABLED_SERVICES_FILE"
    [ ! -x "$w1" ]
    [ ! -x "$w2" ]
    # stop was actually invoked on each.
    grep -q "stop" "$BATS_TEST_TMPDIR/webrtc.calls"
    grep -q "stop" "$BATS_TEST_TMPDIR/webrtc_local.calls"
}

@test "recorded webrtc disables are reversed by reenable_disabled_services" {
    unset _HELIX_UNINSTALL_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"

    local w1 w2
    w1=$(make_fake_webrtc webrtc)
    w2=$(make_fake_webrtc webrtc_local)
    chmod a-x "$w1" "$w2"
    record_disabled_service "sysv-chmod" "$w1"
    record_disabled_service "sysv-chmod" "$w2"

    reenable_disabled_services
    [ -x "$w1" ]
    [ -x "$w2" ]
}

# ---------------------------------------------------------------------------
# Webcam backup recording
# ---------------------------------------------------------------------------

@test "_record_webcam_backup writes backup + marker once and never overwrites" {
    _record_webcam_backup '[{"name":"Default","service":"iframe"}]'
    [ -f "$WEBCAM_BACKUP" ]
    [ -f "$CAMERA_MARKER" ]
    grep -q '"Default"' "$WEBCAM_BACKUP"

    # Second call with different content must NOT overwrite the first (true stock).
    _record_webcam_backup '[{"name":"Changed"}]'
    grep -q '"Default"' "$WEBCAM_BACKUP"
    ! grep -q '"Changed"' "$WEBCAM_BACKUP"
}

# ---------------------------------------------------------------------------
# LAN IP detection
# ---------------------------------------------------------------------------

@test "detect_lan_ip: parses src field from 'ip route get'" {
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 via 192.168.1.1 dev eth0 src 192.168.1.74 uid 0";;
        *) exit 0;;
    esac'
    run detect_lan_ip
    [ "$status" -eq 0 ]
    [ "$output" = "192.168.1.74" ]
}

@test "detect_lan_ip: falls back to default-route iface inet address" {
    mock_command_script "ip" 'case "$1 $2" in
        "route get") exit 0;;  # no src field
        "route 2>/dev/null"|"route ") : ;;
    esac
    case "$*" in
        "route get 1.1.1.1") echo "broken output no src";;
        "route") echo "default via 192.168.30.1 dev wlan0";;
        "-4 addr show wlan0") echo "    inet 192.168.30.193/24 brd 192.168.30.255 scope global wlan0";;
        *) exit 0;;
    esac'
    run detect_lan_ip
    [ "$status" -eq 0 ]
    [ "$output" = "192.168.30.193" ]
}

@test "detect_lan_ip: empty when nothing resolves" {
    mock_command_script "ip" 'exit 0'
    # Stub ifconfig too: the system PATH stays on the front of $PATH after
    # mock_command_script, so a host with net-tools (e.g. the CI runners)
    # would otherwise hit detect_lan_ip's ifconfig fallback and return the
    # runner's real IP. Stubbing it empty makes "nothing resolves" deterministic.
    mock_command_script "ifconfig" 'exit 0'
    run detect_lan_ip
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# Full install against a fake Moonraker (no real /etc/init.d): exercises the
# webcam migration + backup + marker. The /etc/init.d/ustreamer steps will
# no-op (no rc.common), which is fine — we assert the moonraker-facing reversal
# state, which is the load-bearing part.
# ---------------------------------------------------------------------------

@test "install: full path installs init, disables webrtc, migrates webcams" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    # Stock WebRTC init scripts present so the device-freeing step engages.
    local w1 w2
    w1=$(make_fake_webrtc webrtc)
    w2=$(make_fake_webrtc webrtc_local)
    # Provide a LAN IP deterministically.
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # The procd ustreamer init was installed + executable.
    [ -x "$HELIX_INITD_DIR/ustreamer" ]

    # Stock WebRTC was stopped, chmod a-x'd, and recorded for reversal.
    [ ! -x "$w1" ]
    [ ! -x "$w2" ]
    grep -q "sysv-chmod:$w1" "$DISABLED_SERVICES_FILE"
    grep -q "sysv-chmod:$w2" "$DISABLED_SERVICES_FILE"

    # Backup of the stock Default webcam was recorded once + marker dropped.
    [ -f "$WEBCAM_BACKUP" ]
    grep -q '"Default"' "$WEBCAM_BACKUP"
    [ -f "$CAMERA_MARKER" ]

    # Moonraker saw a DELETE of Default and a POST registering our ustreamer cam.
    # The webcam is registered as mjpegstreamer-adaptive (not ustreamer) so
    # fluidd/mainsail render it — see the service-type note in camera.sh.
    grep -q 'DELETE /server/webcams/item?name=Default' "$MR_REQUESTS"
    grep -q 'POST /server/webcams/item' "$MR_REQUESTS"
    grep -q 'http://192.168.1.74:8080/stream' "$MR_REQUESTS"
    grep -q '"service": "mjpegstreamer-adaptive"' "$MR_REQUESTS"
}

@test "install: idempotent re-run does not reinstall init or re-record disables" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    local w1 w2
    w1=$(make_fake_webrtc webrtc)
    w2=$(make_fake_webrtc webrtc_local)
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    install_camera_k2 "k2"
    # Re-running with the SAME moonraker state must not duplicate the recorded
    # disables (record_disabled_service dedups) nor error on the existing init.
    # Anchor the match at end-of-line: "webrtc" is a prefix of "webrtc_local".
    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    [ "$(grep -c "sysv-chmod:$w1\$" "$DISABLED_SERVICES_FILE")" -eq 1 ]
    [ "$(grep -c "sysv-chmod:$w2\$" "$DISABLED_SERVICES_FILE")" -eq 1 ]
}

@test "install: idempotent re-run does not duplicate backup or double-record" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    install_camera_k2 "k2"
    cp "$WEBCAM_BACKUP" "$BATS_TEST_TMPDIR/backup1"

    # Second run: Moonraker now reports our ustreamer cam (the no-stomp guard
    # should fire and leave the original backup untouched).
    printf '%s' '[{"name":"HelixScreen Camera","service":"ustreamer","stream_url":"http://192.168.1.74:8080/stream"}]' > "$BATS_TEST_TMPDIR/mr_state.json"
    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    # Backup unchanged (still the original stock list).
    diff "$BATS_TEST_TMPDIR/backup1" "$WEBCAM_BACKUP"
}

# ---------------------------------------------------------------------------
# Uninstall teardown
# ---------------------------------------------------------------------------

@test "uninstall: removes ustreamer binary and restores webcams from backup" {
    skip_if_no_python
    # Simulate a prior install: binary present, marker + backup present.
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    printf '%s' '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]' > "$WEBCAM_BACKUP"
    touch "$CAMERA_MARKER"
    start_fake_moonraker '[{"name":"HelixScreen Camera","service":"ustreamer"}]'

    run uninstall_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # Binary gone.
    [ ! -f "$INSTALL_DIR/bin/ustreamer" ]
    # Our entry deleted, stock Default re-POSTed.
    grep -q 'DELETE /server/webcams/item?name=HelixScreen%20Camera' "$MR_REQUESTS"
    grep -q 'POST /server/webcams/item' "$MR_REQUESTS"
    grep -q '"name": "Default"' "$MR_REQUESTS"
    # Marker + backup cleaned up.
    [ ! -f "$CAMERA_MARKER" ]
    [ ! -f "$WEBCAM_BACKUP" ]
}

@test "uninstall: no marker means webcams are left alone" {
    skip_if_no_python
    rm -f "$CAMERA_MARKER" "$WEBCAM_BACKUP"
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    start_fake_moonraker '[]'

    run uninstall_camera_k2 "k2"
    [ "$status" -eq 0 ]
    [ ! -f "$INSTALL_DIR/bin/ustreamer" ]
    # No webcam REST calls when there was no migration marker.
    [ ! -s "$MR_REQUESTS" ] || ! grep -q 'webcams/item' "$MR_REQUESTS"
}

# ---------------------------------------------------------------------------
# K2-Camera-main community mod: detection + reversible conflict resolution
# ---------------------------------------------------------------------------

@test "_detect_k2_camera_mod: true when the mod source dir exists" {
    export HELIX_K2CAM_DIR="$BATS_TEST_TMPDIR/K2-Camera-main"
    export HELIX_K2CAM_MR_BACKUP="$BATS_TEST_TMPDIR/moonraker_backup"
    mkdir -p "$HELIX_K2CAM_DIR"
    run _detect_k2_camera_mod
    [ "$status" -eq 0 ]
}

@test "_detect_k2_camera_mod: true when only the Moonraker backup exists" {
    export HELIX_K2CAM_DIR="$BATS_TEST_TMPDIR/K2-Camera-main"
    export HELIX_K2CAM_MR_BACKUP="$BATS_TEST_TMPDIR/moonraker_backup"
    mkdir -p "$HELIX_K2CAM_MR_BACKUP"
    run _detect_k2_camera_mod
    [ "$status" -eq 0 ]
}

@test "_detect_k2_camera_mod: false when neither signature is present" {
    export HELIX_K2CAM_DIR="$BATS_TEST_TMPDIR/K2-Camera-main"
    export HELIX_K2CAM_MR_BACKUP="$BATS_TEST_TMPDIR/moonraker_backup"
    run _detect_k2_camera_mod
    [ "$status" -eq 1 ]
}

@test "_disable_k2cam_webcam: comments the [webcam Default] block and is idempotent" {
    # Stub the Moonraker restart so we don't touch the host init system.
    _restart_moonraker() { :; }

    local mr_dir="$BATS_TEST_TMPDIR/usr/share/moonraker"
    mkdir -p "$mr_dir"
    export HELIX_K2CAM_MR_DIR="$mr_dir"
    local conf="$mr_dir/moonraker.conf"
    cat > "$conf" <<'EOF'
[server]
host: 0.0.0.0

[webcam Default]
service: iframe
stream_url: /camera.html
snapshot_url: /snapshot.html

[authorization]
trusted_clients:
EOF

    _disable_k2cam_webcam

    # Every line of the [webcam Default] block (header through the line before
    # the blank line) is now prefixed.
    grep -q '^#helix-k2cam-disabled# \[webcam Default\]' "$conf"
    grep -q '^#helix-k2cam-disabled# service: iframe' "$conf"
    grep -q '^#helix-k2cam-disabled# stream_url: /camera.html' "$conf"
    grep -q '^#helix-k2cam-disabled# snapshot_url: /snapshot.html' "$conf"
    # Adjacent sections are untouched.
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"

    # Marker records the affected conf path.
    grep -qF "$conf" "$(_k2cam_marker_file)"

    # Idempotent: a second pass must not double-comment.
    _disable_k2cam_webcam
    [ "$(grep -c '^#helix-k2cam-disabled# #helix-k2cam-disabled#' "$conf")" -eq 0 ]
    [ "$(grep -c '^#helix-k2cam-disabled# \[webcam Default\]' "$conf")" -eq 1 ]
    # Marker not duplicated.
    [ "$(grep -cF "$conf" "$(_k2cam_marker_file)")" -eq 1 ]
}

@test "uninstall: un-comments the K2-Camera-main [webcam Default] block (round-trip)" {
    _restart_moonraker() { :; }

    local mr_dir="$BATS_TEST_TMPDIR/usr/share/moonraker"
    mkdir -p "$mr_dir"
    export HELIX_K2CAM_MR_DIR="$mr_dir"
    local conf="$mr_dir/moonraker.conf"
    local original
    original=$(cat <<'EOF'
[server]
host: 0.0.0.0

[webcam Default]
service: iframe
stream_url: /camera.html

[authorization]
trusted_clients:
EOF
)
    printf '%s\n' "$original" > "$conf"

    # Disable, then uninstall should restore byte-for-byte.
    _disable_k2cam_webcam
    grep -q '^#helix-k2cam-disabled# \[webcam Default\]' "$conf"

    # No prior ustreamer install state — only the k2cam marker matters here.
    rm -f "$CAMERA_MARKER" "$WEBCAM_BACKUP"

    run uninstall_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # The disable prefix is gone; content matches the original.
    ! grep -q 'helix-k2cam-disabled' "$conf"
    [ "$(cat "$conf")" = "$original" ]
    # Marker cleaned up.
    [ ! -f "$(_k2cam_marker_file)" ]
}

@test "install_camera_k2: detects the mod and disables its [webcam Default] entry" {
    _restart_moonraker() { :; }

    # Mod signature present (source dir).
    export HELIX_K2CAM_DIR="$BATS_TEST_TMPDIR/K2-Camera-main"
    export HELIX_K2CAM_MR_BACKUP="$BATS_TEST_TMPDIR/moonraker_backup_absent"
    mkdir -p "$HELIX_K2CAM_DIR"

    local mr_dir="$BATS_TEST_TMPDIR/usr/share/moonraker"
    mkdir -p "$mr_dir"
    export HELIX_K2CAM_MR_DIR="$mr_dir"
    local conf="$mr_dir/moonraker.conf"
    cat > "$conf" <<'EOF'
[webcam Default]
service: iframe
stream_url: /camera.html
EOF

    # No ustreamer binary -> install returns early after the k2cam step, which is
    # fine: the k2cam disable runs before that early-return.
    rm -f "$INSTALL_DIR/bin/ustreamer"

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    grep -q '^#helix-k2cam-disabled# \[webcam Default\]' "$conf"
    grep -qF "$conf" "$(_k2cam_marker_file)"
}

# ---------------------------------------------------------------------------
# Upgrade migration: ownership-aware no-stomp + init-script convergence
# ---------------------------------------------------------------------------

@test "_moonraker_has_our_webcam: true when our 'HelixScreen Camera' entry exists" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"HelixScreen Camera","service":"ustreamer","stream_url":"http://x/stream"}]'
    run _moonraker_has_our_webcam
    [ "$status" -eq 0 ]
}

@test "_moonraker_has_our_webcam: false when only a differently-named cam exists" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"My Cam","service":"mjpegstreamer","stream_url":"http://x/stream"}]'
    run _moonraker_has_our_webcam
    [ "$status" -eq 1 ]
}

@test "install: upgrade re-registers OUR stale ustreamer cam as mjpegstreamer-adaptive" {
    skip_if_no_python
    # Pre-existing install where our webcam is registered with the STALE service
    # type (ustreamer). It counts as "usable", but it's ours — install must NOT
    # early-return; it must fall through and re-register with the corrected type.
    start_fake_moonraker '[{"name":"HelixScreen Camera","service":"ustreamer","stream_url":"http://192.168.1.74:8080/stream"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # Did NOT early-return: a POST re-registering the corrected service type fired.
    grep -q 'POST /server/webcams/item' "$MR_REQUESTS"
    grep -q '"service": "mjpegstreamer-adaptive"' "$MR_REQUESTS"
}

@test "install: leaves a usable THIRD-PARTY camera untouched (early-return)" {
    skip_if_no_python
    # Usable cam that is NOT ours — install must early-return and touch nothing.
    start_fake_moonraker '[{"name":"My Cam","service":"mjpegstreamer","stream_url":"http://x/stream"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]
    # No webcam REST mutation (only the GETs from the two detect probes).
    ! grep -q 'POST /server/webcams/item' "$MR_REQUESTS"
    ! grep -q 'DELETE /server/webcams/item' "$MR_REQUESTS"
    # No install side effects.
    [ ! -f "$DISABLED_SERVICES_FILE" ]
    [ ! -f "$WEBCAM_BACKUP" ]
    [ ! -f "$CAMERA_MARKER" ]
    [ ! -f "$HELIX_INITD_DIR/ustreamer" ]
}

@test "install: init script is overwritten when an existing one differs" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    # Stale init script on disk whose content differs from the bundled source.
    printf '#!/bin/sh\n# OLD STALE VERSION\n' > "$HELIX_INITD_DIR/ustreamer"
    chmod +x "$HELIX_INITD_DIR/ustreamer"

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # It was migrated to match the current bundled source byte-for-byte.
    cmp -s "$INSTALL_DIR/config/helixscreen-ustreamer-k2.sh" "$HELIX_INITD_DIR/ustreamer"
    ! grep -q 'OLD STALE VERSION' "$HELIX_INITD_DIR/ustreamer"
}

@test "install: init script is NOT rewritten when already current" {
    skip_if_no_python
    start_fake_moonraker '[{"name":"Default","service":"iframe","stream_url":"http://k2/webrtc"}]'
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/ustreamer"; chmod +x "$INSTALL_DIR/bin/ustreamer"
    mock_command_script "ip" 'case "$*" in
        "route get 1.1.1.1") echo "1.1.1.1 dev eth0 src 192.168.1.74";;
        *) exit 0;;
    esac'

    # Pre-seed the init dest with content IDENTICAL to the bundled source. The
    # install uses `$SUDO cp` (SUDO="" in tests, so a bare `cp`); stub `cp` to
    # record any invocation that writes the init dest. cmp -s should match, so
    # the install must take the "already current" branch and never call cp.
    cp "$INSTALL_DIR/config/helixscreen-ustreamer-k2.sh" "$HELIX_INITD_DIR/ustreamer"
    chmod +x "$HELIX_INITD_DIR/ustreamer"

    local cp_log="$BATS_TEST_TMPDIR/cp.log"
    : > "$cp_log"
    cp() {
        echo "cp $*" >> "$cp_log"
        command cp "$@"
    }
    export -f cp

    run install_camera_k2 "k2"
    [ "$status" -eq 0 ]

    # The init dest was never re-copied (no `cp ... $HELIX_INITD_DIR/ustreamer`).
    ! grep -q "$HELIX_INITD_DIR/ustreamer" "$cp_log"
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

skip_if_no_python() {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
}

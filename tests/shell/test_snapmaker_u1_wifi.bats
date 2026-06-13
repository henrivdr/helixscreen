#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the Snapmaker U1 WiFi-decouple logic in hooks-snapmaker-u1.sh.
#
# The stock Snapmaker UI (which HelixScreen replaces) is what loads the user's
# saved WiFi credentials into wpa_supplicant at runtime. With the stock UI
# disabled, nothing restores WiFi unless HelixScreen does. The old hook tried
# once, synchronously, in platform_pre_start — racing wpa_supplicant/wlan0 at
# early boot and silently failing, AND coupling network recovery to helix's
# lifetime so a momentary helix death stranded the device off-network.
#
# The fix: a synchronous, idempotent apply (_helix_wifi_apply_saved) wrapped in
# a DETACHED, retrying worker (ensure_wifi_associated) that outlives helix.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
HOOK="$WORKTREE_ROOT/assets/config/platform/hooks-snapmaker-u1.sh"

setup() {
    load helpers

    export SAVED_CONF="$BATS_TEST_TMPDIR/wpa_supplicant.conf"
    export WPA_LOG="$BATS_TEST_TMPDIR/wpa_cli_calls.log"

    # Point the hook at our temp paths instead of the device locations.
    export HELIX_SAVED_WPA="$SAVED_CONF"
    export HELIX_WIFI_FLAG="$BATS_TEST_TMPDIR/wifi-restore.active"
    export HELIX_WIFI_IFACE="wlan0"

    cat > "$SAVED_CONF" <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
network={
    ssid="MyNet"
    psk="secretpass"
}
EOF
}

# Mock wpa_cli. STATUS_STATE controls what `status` reports; every call is
# appended to WPA_LOG so tests can assert what the hook drove.
mock_wpa_cli() {
    local status_state="$1"  # e.g. SCANNING or COMPLETED
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/wpa_cli" <<EOF
#!/bin/sh
echo "wpa_cli \$*" >> "$WPA_LOG"
# args: -i <iface> <cmd> ...
cmd="\$3"
case "\$cmd" in
    status)      echo "wpa_state=$status_state" ;;
    add_network) echo "0" ;;
    *)           echo "OK" ;;
esac
exit 0
EOF
    chmod +x "$BATS_TEST_TMPDIR/bin/wpa_cli"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

@test "u1 hook: _helix_wifi_apply_saved pushes the saved network via wpa_cli" {
    mock_wpa_cli SCANNING   # not yet connected → must apply
    rm -f "$WPA_LOG"

    ( . "$HOOK"; _helix_wifi_apply_saved )

    [ -f "$WPA_LOG" ]
    grep -q 'add_network' "$WPA_LOG"
    grep -q 'set_network 0 ssid "MyNet"' "$WPA_LOG"
    grep -q 'set_network 0 psk "secretpass"' "$WPA_LOG"
    grep -q 'select_network 0' "$WPA_LOG"
}

@test "u1 hook: apply is a no-op when already associated (COMPLETED)" {
    mock_wpa_cli COMPLETED
    rm -f "$WPA_LOG"

    ( . "$HOOK"; _helix_wifi_apply_saved )

    # It checked status but must NOT have reconfigured an already-up link.
    grep -q 'status' "$WPA_LOG"
    ! grep -q 'add_network' "$WPA_LOG"
    ! grep -q 'select_network' "$WPA_LOG"
}

@test "u1 hook: apply does nothing when there is no saved config" {
    mock_wpa_cli SCANNING
    rm -f "$SAVED_CONF" "$WPA_LOG"

    run sh -c ". \"$HOOK\"; _helix_wifi_apply_saved"
    [ "$status" -ne 0 ]          # returns non-zero: nothing to apply
    [ ! -f "$WPA_LOG" ]          # wpa_cli never invoked
}

@test "u1 hook: ensure_wifi_associated returns promptly and drives the restore (detached)" {
    # Status never reaches COMPLETED so the worker exhausts its retries; sleep is
    # a no-op so the detached worker finishes near-instantly instead of blocking
    # the test. The point under test: ensure_wifi_associated does the recovery in
    # a background worker (decoupled from helix) and the wpa_cli restore runs.
    mock_wpa_cli SCANNING
    mock_command_script "sleep" 'exit 0'
    rm -f "$WPA_LOG"

    run sh -c ". \"$HOOK\"; ensure_wifi_associated"
    [ "$status" -eq 0 ]

    # Give the detached worker a beat to run (sleeps are no-ops, so this is ample).
    sleep 1
    grep -q 'add_network' "$WPA_LOG"
}

@test "u1 hook: ensure_wifi_associated is a no-op when already connected" {
    mock_wpa_cli COMPLETED
    rm -f "$WPA_LOG"

    run sh -c ". \"$HOOK\"; ensure_wifi_associated"
    [ "$status" -eq 0 ]
    sleep 1
    ! grep -q 'add_network' "$WPA_LOG"
}

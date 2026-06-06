#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the generic per-printer settings.json seed mechanism (GitHub #986).
# Exercises seed_settings_for_printer() in printer_seed.sh:
#   - deep-merging a bundled seed fragment into the install's settings.json
#   - existing user keys winning over the fragment (fragment is lower priority)
#   - a missing fragment being a graceful no-op
#   - graceful skip when python3 is unavailable
#   - recording the seed action to a state file for uninstall awareness
#
# Also exercises detect_printer_model() conservatism (no false positives).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Temporary install directory + a fake bundled-seeds dir we control.
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export SETTINGS_FILE="$INSTALL_DIR/config/settings.json"
    export SEED_STATE_FILE="$INSTALL_DIR/config/.seeded_settings"

    # Point the module at a test-controlled seed-fragment directory so we are
    # not coupled to the shipped config/install_seeds/*.json contents.
    export HELIX_SEED_DIR="$BATS_TEST_TMPDIR/install_seeds"
    mkdir -p "$HELIX_SEED_DIR"

    SUDO=""
    export SUDO

    unset _HELIX_PRINTER_SEED_SOURCED
    # printer_seed.sh uses file_sudo() from common.sh
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/printer_seed.sh"
}

# Write a seed fragment for a given id into the test seed dir.
write_fragment() {
    local id="$1" body="$2"
    printf '%s\n' "$body" > "$HELIX_SEED_DIR/${id}.json"
}

@test "seed: merges fragment into absent settings.json" {
    rm -f "$SETTINGS_FILE"
    write_fragment "demo" '{"input":{"calibration":{"valid":true,"a":1.66}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    [ -f "$SETTINGS_FILE" ]

    # The fragment values landed.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['valid'] is True;assert d['input']['calibration']['a']==1.66"
}

@test "seed: merges fragment into empty settings.json" {
    printf '{}\n' > "$SETTINGS_FILE"
    write_fragment "demo" '{"input":{"calibration":{"valid":true}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['valid'] is True"
}

@test "seed: existing user keys win over the fragment" {
    # User already calibrated with their own values; the seed must NOT clobber them.
    printf '%s\n' '{"input":{"calibration":{"valid":true,"a":9.99}},"foo":"bar"}' > "$SETTINGS_FILE"
    write_fragment "demo" '{"input":{"calibration":{"valid":true,"a":1.66,"c":0.0}},"baz":"qux"}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]

    # Existing a=9.99 preserved; existing foo preserved; new keys (c, baz) added.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['a']==9.99, d;assert d['foo']=='bar';assert d['input']['calibration']['c']==0.0;assert d['baz']=='qux'"
}

@test "seed: missing fragment is a graceful no-op success" {
    printf '%s\n' '{"foo":"bar"}' > "$SETTINGS_FILE"

    run seed_settings_for_printer "no_such_printer"
    [ "$status" -eq 0 ]

    # settings.json untouched.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d=={'foo':'bar'}, d"
    # Nothing recorded.
    [ ! -f "$SEED_STATE_FILE" ] || ! grep -q "no_such_printer" "$SEED_STATE_FILE"
}

@test "seed: records the seeded id to the state file" {
    write_fragment "demo" '{"input":{"calibration":{"valid":true}}}'
    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    [ -f "$SEED_STATE_FILE" ]
    grep -q "demo" "$SEED_STATE_FILE"
}

@test "seed: python3 absent path skips gracefully (no failure)" {
    write_fragment "demo" '{"input":{"calibration":{"valid":true}}}'

    # Shadow python3 by pointing PATH at an empty dir so the module's
    # `command -v python3` lookup misses. Save/restore PATH around the call.
    local empty_bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$empty_bin"
    local saved_path="$PATH"
    PATH="$empty_bin"
    run seed_settings_for_printer "demo"
    PATH="$saved_path"
    [ "$status" -eq 0 ]
    # settings.json must not have been created by the skipped merge.
    [ ! -f "$SETTINGS_FILE" ]
}

# --- detect_printer_model() conservatism (stubbed detection, no false positives) ---

@test "detect: returns empty on a plain non-matching environment" {
    # No mksclient, generic hostname → must not misfire to sovol_sv06_ace.
    local empty_bin="$BATS_TEST_TMPDIR/det_empty"
    mkdir -p "$empty_bin"
    local saved_path="$PATH"
    PATH="$empty_bin"
    HELIX_FAKE_HOSTNAME="raspberrypi" run detect_printer_model
    PATH="$saved_path"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the per-printer settings.json seed mechanism (GitHub #986).
# Exercises seed_settings_for_printer() in printer_seed.sh:
#   - the seed source is now the runtime preset assets/config/presets/<id>.json
#   - only the install-time pre-seed allowlist (input.calibration) is baked into
#     settings.json; the rest of the preset (heaters/fans/leds/...) is NOT
#   - nested "_"-prefixed provenance keys are stripped from the seeded subset
#   - existing user keys win over the seeded calibration (preset is lower prio)
#   - a missing preset being a graceful no-op
#   - graceful skip when python3 is unavailable
#   - recording the seed action to a state file for uninstall awareness
#
# Also exercises detect_printer_model() conservatism (no false positives).
#
# Independent of the shipped preset contents: writes preset-shaped JSON into a
# temp HELIX_PRESET_DIR. (The wiring test deliberately uses the real preset.)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Temporary install directory + a fake preset dir we control.
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export SETTINGS_FILE="$INSTALL_DIR/config/settings.json"
    export SEED_STATE_FILE="$INSTALL_DIR/config/.seeded_settings"

    # Point the module at a test-controlled preset directory so we are not
    # coupled to the shipped assets/config/presets/*.json contents.
    export HELIX_PRESET_DIR="$BATS_TEST_TMPDIR/presets"
    mkdir -p "$HELIX_PRESET_DIR"

    SUDO=""
    export SUDO

    unset _HELIX_PRINTER_SEED_SOURCED
    # printer_seed.sh uses file_sudo() from common.sh
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/printer_seed.sh"
}

# Write a preset for a given id into the test preset dir.
write_preset() {
    local id="$1" body="$2"
    printf '%s\n' "$body" > "$HELIX_PRESET_DIR/${id}.json"
}

@test "seed: seeds ONLY the allowlist (input.calibration) into absent settings.json" {
    rm -f "$SETTINGS_FILE"
    # Preset-shaped: calibration (allowlisted) + hardware mappings (NOT).
    write_preset "demo" '{"preset":"demo","input":{"calibration":{"valid":true,"a":1.66,"e":1.76}},"printer":{"heaters":{"bed":"heater_bed"},"leds":{"selected_strips":["x"]}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    [ -f "$SETTINGS_FILE" ]

    # The allowlisted calibration landed.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));c=d['input']['calibration'];assert c['valid'] is True;assert c['a']==1.66;assert c['e']==1.76"
    # The non-allowlist preset keys did NOT.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert 'printer' not in d, d;assert 'preset' not in d, d"
}

@test "seed: nested _comment in calibration is stripped from settings.json" {
    rm -f "$SETTINGS_FILE"
    write_preset "demo" '{"input":{"calibration":{"_comment":"provenance blurb","valid":true,"a":1.66}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]

    # Real calibration landed, but the nested provenance _comment did not.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));c=d['input']['calibration'];assert c['valid'] is True;assert c['a']==1.66;assert '_comment' not in c, c"
}

@test "seed: top-level _ provenance keys never reach settings.json" {
    # A preset's top-level "_*" metadata (e.g. _todo_986_comment) is outside the
    # allowlist, so it is never extracted — defense-in-depth plus extraction.
    rm -f "$SETTINGS_FILE"
    write_preset "demo" '{"_todo_986_comment":"deferred stuff","input":{"calibration":{"valid":true}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]

    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['valid'] is True;assert '_todo_986_comment' not in d, d"
}

@test "seed: merges allowlist into empty settings.json" {
    printf '{}\n' > "$SETTINGS_FILE"
    write_preset "demo" '{"input":{"calibration":{"valid":true}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['valid'] is True"
}

@test "seed: existing user calibration wins over the preset" {
    # User already calibrated with their own values; the seed must NOT clobber.
    printf '%s\n' '{"input":{"calibration":{"valid":true,"a":9.99}},"foo":"bar"}' > "$SETTINGS_FILE"
    write_preset "demo" '{"input":{"calibration":{"valid":true,"a":1.66,"c":0.0}}}'

    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]

    # Existing a=9.99 preserved; existing foo preserved; new key c added.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['a']==9.99, d;assert d['foo']=='bar';assert d['input']['calibration']['c']==0.0"
}

@test "seed: missing preset is a graceful no-op success" {
    printf '%s\n' '{"foo":"bar"}' > "$SETTINGS_FILE"

    run seed_settings_for_printer "no_such_printer"
    [ "$status" -eq 0 ]

    # settings.json untouched.
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d=={'foo':'bar'}, d"
    # Nothing recorded.
    [ ! -f "$SEED_STATE_FILE" ] || ! grep -q "no_such_printer" "$SEED_STATE_FILE"
}

@test "seed: records the seeded id to the state file" {
    write_preset "demo" '{"input":{"calibration":{"valid":true}}}'
    run seed_settings_for_printer "demo"
    [ "$status" -eq 0 ]
    [ -f "$SEED_STATE_FILE" ]
    grep -q "demo" "$SEED_STATE_FILE"
}

@test "seed: python3 absent path skips gracefully (no failure)" {
    write_preset "demo" '{"input":{"calibration":{"valid":true}}}'

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

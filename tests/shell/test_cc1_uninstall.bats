#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the COSMOS-specific restore block in uninstall() (uninstall.sh).
# Mirrors the install path covered by test_cc1_competing_uis.bats: confirms each
# wrapped sibling (grumpyscreen/guppyscreen/atomscreen) is restored from its
# .helix-bak, a stale backup (live file already stock) is dropped, and cosmos.conf
# is only reverted when it still holds the legacy 'helixscreen' value.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

# Post-install state for ONE sibling: live file is our wrapper, .helix-bak holds
# the stock original.
_make_wrapped_sibling() {
    local name="$1"
    cat > "$MOCK_ROOT/etc/init.d/$name.helix-bak" <<EOF
#!/bin/sh
# Stock COSMOS $name (mocked)
echo "stock $name \$1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/$name.helix-bak"

    cat > "$MOCK_ROOT/etc/init.d/$name" <<'EOF'
#!/bin/sh
# HELIXSCREEN_WRAPPER
exec /etc/init.d/helixscreen "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/$name"
}

setup() {
    load helpers

    log_info() { echo "INFO: $*"; }
    log_warn() { echo "WARN: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_success

    export MOCK_ROOT="$BATS_TEST_TMPDIR/cc1"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/etc/klipper/config" "$MOCK_ROOT/usr/bin"

    # Marker file the uninstaller uses to detect COSMOS
    touch "$MOCK_ROOT/usr/bin/update-cosmos"
    chmod +x "$MOCK_ROOT/usr/bin/update-cosmos"

    # Simulate post-install state for grumpyscreen (the others are added per-test
    # where the wider-restore behavior is exercised).
    _make_wrapped_sibling grumpyscreen

    cat > "$MOCK_ROOT/etc/klipper/config/cosmos.conf" <<'EOF'
[ui]
screen_ui = helixscreen
web_ui = mainsail
EOF

    # The function is large; we only exercise the COSMOS restore block by
    # re-running the same path-rewriting trick used in test_cc1_competing_uis.
    # We extract just the COSMOS branch into a callable function so we don't
    # need to mock the entire uninstall (init scripts, install dirs, etc.).
    cat > "$BATS_TEST_TMPDIR/cosmos_restore.sh" <<'SH_EOF'
#!/bin/sh
SH_EOF

    # Pull the contiguous COSMOS restore block. The block now contains a
    # `for ... done` loop with an inner `if ... elif ... fi` at 12-space indent
    # and two more `if ... fi` at 8-space indent; only the OUTER `fi` is at 4
    # spaces, so `^    fi$` matches it (and only it) — we exit on the first hit.
    awk '
        /^    # COSMOS \(Centauri Carbon\)/ { capture=1 }
        capture { print }
        capture && /^    fi$/ && ++blocks==1 { exit }
    ' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" >> "$BATS_TEST_TMPDIR/cosmos_restore.sh"

    # Directory-prefix rewrite: the block builds sibling paths dynamically as
    # /etc/init.d/${_sib}, so a per-name literal rewrite would miss them.
    sed -i \
        -e "s|/etc/init.d|$MOCK_ROOT/etc/init.d|g" \
        -e "s|/etc/klipper|$MOCK_ROOT/etc/klipper|g" \
        -e "s|/usr/bin/update-cosmos|$MOCK_ROOT/usr/bin/update-cosmos|g" \
        "$BATS_TEST_TMPDIR/cosmos_restore.sh"

    # Safety net: extracted block must operate entirely under MOCK_ROOT.
    ! grep -qE '(^|[^A-Za-z0-9_.-])/etc/init\.d' "$BATS_TEST_TMPDIR/cosmos_restore.sh"
    ! grep -qE '(^|[^A-Za-z0-9_.-])/etc/klipper' "$BATS_TEST_TMPDIR/cosmos_restore.sh"

    cosmos_restore() {
        local restored_ui=""
        # shellcheck disable=SC1091
        . "$BATS_TEST_TMPDIR/cosmos_restore.sh"
        printf '%s' "$restored_ui"
    }
}

@test "uninstall cc1: restores stock grumpyscreen from .helix-bak" {
    run cosmos_restore
    [ "$status" -eq 0 ]
    [ -f "$MOCK_ROOT/etc/init.d/grumpyscreen" ]
    [ ! -f "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak" ]
    grep -q "Stock COSMOS grumpyscreen" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/grumpyscreen"
}

@test "uninstall cc1: restores all three wrapped siblings from their backups" {
    _make_wrapped_sibling guppyscreen
    _make_wrapped_sibling atomscreen
    run cosmos_restore
    [ "$status" -eq 0 ]
    for s in grumpyscreen guppyscreen atomscreen; do
        [ -f "$MOCK_ROOT/etc/init.d/$s" ]
        [ ! -f "$MOCK_ROOT/etc/init.d/$s.helix-bak" ]
        grep -q "Stock COSMOS $s" "$MOCK_ROOT/etc/init.d/$s"
        ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/$s"
    done
}

@test "uninstall cc1: drops a stale backup when live file is already stock" {
    # Simulate a COSMOS upgrade that already restored the stock guppyscreen over
    # our wrapper, leaving a now-redundant .helix-bak behind.
    cat > "$MOCK_ROOT/etc/init.d/guppyscreen" <<'EOF'
#!/bin/sh
# Stock COSMOS guppyscreen (restored by upgrade)
echo "stock guppyscreen $1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/guppyscreen"
    cat > "$MOCK_ROOT/etc/init.d/guppyscreen.helix-bak" <<'EOF'
#!/bin/sh
# Stock COSMOS guppyscreen (mocked)
echo "stock guppyscreen $1"
EOF

    run cosmos_restore
    [ "$status" -eq 0 ]
    # Redundant backup removed...
    [ ! -f "$MOCK_ROOT/etc/init.d/guppyscreen.helix-bak" ]
    # ...and the live stock file is left intact (not overwritten / not deleted).
    [ -f "$MOCK_ROOT/etc/init.d/guppyscreen" ]
    grep -q "restored by upgrade" "$MOCK_ROOT/etc/init.d/guppyscreen"
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/guppyscreen"
}

@test "uninstall cc1: reverts cosmos.conf screen_ui to grumpyscreen" {
    cosmos_restore >/dev/null
    grep -q "^screen_ui = grumpyscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
    ! grep -q "screen_ui = helixscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
}

@test "uninstall cc1: leaves cosmos.conf alone if value isn't the legacy helixscreen" {
    # We only revert the legacy invalid 'helixscreen' value. A sibling value the
    # operator chose must be respected (all three are restored to stock anyway).
    sed -i 's|^screen_ui = helixscreen|screen_ui = guppyscreen|' \
        "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
    cosmos_restore >/dev/null
    grep -q "^screen_ui = guppyscreen" "$MOCK_ROOT/etc/klipper/config/cosmos.conf"
}

@test "uninstall cc1: missing .helix-bak does not crash, leaves wrapper as-is" {
    # Edge case: bad install left no backup. Uninstaller must not crash and
    # must not leave the device with no /etc/init.d/grumpyscreen at all.
    rm -f "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak"
    run cosmos_restore
    [ "$status" -eq 0 ]
    [ -f "$MOCK_ROOT/etc/init.d/grumpyscreen" ]
}

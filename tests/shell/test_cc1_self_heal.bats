#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helix_platform_reassert_wrappers() in hooks-cc1.sh — the boot-time
# self-heal that re-asserts a clobbered COSMOS sibling gui-switcher wrapper after
# an upgrade restores the stock init script over our overlay copy.
#
# Conservative by design: it ONLY repairs a sibling when a .helix-bak backup
# already exists (the installer wrapped it once) AND the live file lacks the
# HELIXSCREEN_WRAPPER marker. It never creates a fresh backup, never touches a
# sibling that was never wrapped, and is CC1-guarded by /usr/bin/update-cosmos.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

# Stock (non-wrapper) sibling init script.
_make_stock_sibling() {
    local name="$1"
    cat > "$MOCK_ROOT/etc/init.d/$name" <<EOF
#!/bin/sh
# Stock COSMOS $name init (mocked)
echo "stock $name \$1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/$name"
}

# Our delegating wrapper (already-installed state).
_make_wrapper_sibling() {
    local name="$1"
    cat > "$MOCK_ROOT/etc/init.d/$name" <<'EOF'
#!/bin/sh
# HELIXSCREEN_WRAPPER (do not remove this marker — used for idempotency)
exec /etc/init.d/helixscreen "$@"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/$name"
}

# A .helix-bak holding the stock original.
_make_backup() {
    local name="$1"
    cat > "$MOCK_ROOT/etc/init.d/$name.helix-bak" <<EOF
#!/bin/sh
# Stock COSMOS $name init (mocked)
echo "stock $name \$1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/$name.helix-bak"
}

setup() {
    load helpers

    export MOCK_ROOT="$BATS_TEST_TMPDIR/cc1"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin"

    # CC1 fingerprint: the COSMOS updater. The guard is `[ -x /usr/bin/update-cosmos ]`
    # with an ABSOLUTE path, so the prefix rewrite below must redirect it too.
    cat > "$MOCK_ROOT/usr/bin/update-cosmos" <<'EOF'
#!/bin/sh
exit 0
EOF
    chmod +x "$MOCK_ROOT/usr/bin/update-cosmos"

    # HelixScreen init script the wrapper delegates to.
    cat > "$MOCK_ROOT/etc/init.d/helixscreen" <<'EOF'
#!/bin/sh
echo "helixscreen $1"
EOF
    chmod +x "$MOCK_ROOT/etc/init.d/helixscreen"

    # Read hooks-cc1.sh, redirect absolute path PREFIXES to MOCK_ROOT, source it.
    # The directory-prefix rewrite catches the dynamic /etc/init.d/${s} paths and
    # the absolute /usr/bin/update-cosmos guard, plus the wrapper heredoc's
    # `exec /etc/init.d/helixscreen "$@"`.
    local patched="$BATS_TEST_TMPDIR/hooks-cc1.sh"
    sed -e "s|/etc/init.d|$MOCK_ROOT/etc/init.d|g" \
        -e "s|/usr/bin/update-cosmos|$MOCK_ROOT/usr/bin/update-cosmos|g" \
        "$WORKTREE_ROOT/assets/config/platform/hooks-cc1.sh" > "$patched"

    # Safety net: the rewritten copy must not touch the real /etc tree.
    ! grep -qE '(^|[^A-Za-z0-9_.-])/etc/init\.d' "$patched"

    # shellcheck disable=SC1090
    . "$patched"
}

@test "self-heal: clobbered wrapper (stock live + backup present) is re-wrapped" {
    _make_stock_sibling grumpyscreen   # upgrade restored stock over our overlay
    _make_backup grumpyscreen          # but the installer's backup survives
    run helix_platform_reassert_wrappers
    [ "$status" -eq 0 ]
    grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    grep -q "exec .*/etc/init.d/helixscreen" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    [ -x "$MOCK_ROOT/etc/init.d/grumpyscreen" ]
    # Self-heal must NOT touch or overwrite the backup.
    grep -q "stock grumpyscreen" "$MOCK_ROOT/etc/init.d/grumpyscreen.helix-bak"
}

@test "self-heal: stock sibling with NO backup is left untouched" {
    # Never wrapped by the installer (no .helix-bak) → self-heal must not hijack it.
    _make_stock_sibling guppyscreen
    run helix_platform_reassert_wrappers
    [ "$status" -eq 0 ]
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/guppyscreen"
    grep -q "stock guppyscreen" "$MOCK_ROOT/etc/init.d/guppyscreen"
    [ ! -e "$MOCK_ROOT/etc/init.d/guppyscreen.helix-bak" ]
}

@test "self-heal: already-wrapped sibling is a no-op (content unchanged)" {
    _make_wrapper_sibling atomscreen
    _make_backup atomscreen
    cp "$MOCK_ROOT/etc/init.d/atomscreen" "$MOCK_ROOT/before"
    run helix_platform_reassert_wrappers
    [ "$status" -eq 0 ]
    diff "$MOCK_ROOT/before" "$MOCK_ROOT/etc/init.d/atomscreen"
}

@test "self-heal: non-CC1 (no update-cosmos) is a no-op on everything" {
    rm -f "$MOCK_ROOT/usr/bin/update-cosmos"
    _make_stock_sibling grumpyscreen
    _make_backup grumpyscreen          # backup present, but guard must short-circuit
    run helix_platform_reassert_wrappers
    [ "$status" -eq 0 ]
    # Guard failed → stock file left in place, NOT re-wrapped.
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    grep -q "stock grumpyscreen" "$MOCK_ROOT/etc/init.d/grumpyscreen"
}

@test "self-heal: re-asserted wrapper execs into helixscreen init" {
    _make_stock_sibling grumpyscreen
    _make_backup grumpyscreen
    helix_platform_reassert_wrappers
    # gui-switcher's launch path: running the sibling reaches helixscreen.
    run "$MOCK_ROOT/etc/init.d/grumpyscreen" start
    [ "$status" -eq 0 ]
    [[ "$output" == *"helixscreen start"* ]]
}

@test "self-heal: repairs only the clobbered sibling, leaves others alone" {
    # grumpyscreen clobbered (stock + backup) → repaired.
    _make_stock_sibling grumpyscreen
    _make_backup grumpyscreen
    # guppyscreen still correctly wrapped → untouched.
    _make_wrapper_sibling guppyscreen
    _make_backup guppyscreen
    # atomscreen never wrapped (stock, no backup) → untouched.
    _make_stock_sibling atomscreen

    run helix_platform_reassert_wrappers
    [ "$status" -eq 0 ]
    grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/grumpyscreen"
    grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/guppyscreen"
    ! grep -q "HELIXSCREEN_WRAPPER" "$MOCK_ROOT/etc/init.d/atomscreen"
}

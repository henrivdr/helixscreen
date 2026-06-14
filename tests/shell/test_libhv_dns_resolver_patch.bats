#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regression tests for the libhv DNS resolver fallback patch.
#
# Why this exists: HelixScreen's embedded targets (AD5M, SonicPad, K1/K1C,
# MIPS, AD5X, CC1) ship FULLY-STATIC glibc binaries. A static glibc binary
# cannot reliably dlopen the NSS modules (libnss_dns/libnss_files) that
# getaddrinfo() needs at runtime — on glibc-version-mismatched devices it fails
# or SIGSEGVs (#700). patches/libhv-dns-resolver-fallback.patch wires a direct
# UDP DNS resolver into libhv's ResolveAddr() to bypass NSS entirely.
#
# The patch has THREE hunks: two NEW files (base/dns_resolv.{c,h}) plus a
# modification to base/hsocket.c (the wiring that actually CALLS the resolver).
# An earlier mk/patches.mk guard keyed application on the ABSENCE of
# dns_resolv.c. When a submodule reset reverted the tracked hsocket.c but left
# the untracked dns_resolv.c orphaned, the guard saw the orphan and concluded
# "already applied" — so it never re-wired hsocket.c. Every build since shipped
# binaries where the resolver was compiled but NEVER CALLED, silently breaking
# update checks, telemetry, and debug-bundle uploads on every static-glibc
# device. The unit tests (test_dns_resolver.cpp) exercise the resolver
# functions in isolation and so could not catch this — the gap was the wiring.
# These tests lock both the patch contents and the self-healing guard.

setup() {
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    PATCH="$REPO_ROOT/patches/libhv-dns-resolver-fallback.patch"
    HSOCKET="$REPO_ROOT/lib/libhv/base/hsocket.c"
    PATCHES_MK="$REPO_ROOT/mk/patches.mk"
    RULES_MK="$REPO_ROOT/mk/rules.mk"
}

@test "libhv.a is rebuilt when patches change (depends on PATCHES_STAMP)" {
    # The resolver wiring lives in base/hsocket.c, which is compiled ONLY into
    # libhv.a. If the libhv.a build target has no prerequisite on the patch
    # stamp, the archive is built once and never rebuilt when a patch is
    # (re)applied — leaving a stale, pristine hsocket.o (resolver compiled into
    # the app but never CALLED → getaddrinfo() EAI_SYSTEM on static glibc). This
    # is dev-only (CI builds libhv.a fresh after patches), but it cost a full
    # debugging session. Lock the dependency.
    grep -qE '^\$\(LIBHV_LIB\):[[:space:]]*\$\(PATCHES_STAMP\)' "$RULES_MK"
}

@test "DNS resolver fallback patch file exists" {
    [ -f "$PATCH" ]
}

@test "patch contains the hsocket.c wiring hunk, not only the new files" {
    # The integration hunk is the part that regressed; without it the resolver
    # source is dead code and the binary falls back to the crashy getaddrinfo().
    grep -q 'base/hsocket.c' "$PATCH"
    grep -q 'dns_resolv_resolve' "$PATCH"
}

@test "patch keeps the embedded ARM/MIPS fail-closed guard" {
    # On embedded 32-bit glibc, falling through to getaddrinfo() after a failed
    # UDP resolve can SIGSEGV inside gaih_inet. The patch must fail closed
    # (return -1) rather than call getaddrinfo() on those arches.
    grep -q '__arm__' "$PATCH"
    grep -q '__mips__' "$PATCH"
}

@test "patches.mk guard keys on the hsocket.c wiring, not the orphan file" {
    # Lock the fix: the guard must detect application by grepping hsocket.c for
    # the wiring marker.
    grep -qF 'grep -q "dns_resolv_resolve" "$(LIBHV_DIR)/base/hsocket.c"' "$PATCHES_MK"
}

@test "patches.mk does NOT reintroduce the broken absence-of-file detection" {
    # The original bug: '[ ! -f .../dns_resolv.c ]' mis-read a half-applied
    # state as fully applied. It must never come back.
    ! grep -qF '! -f "$(LIBHV_DIR)/base/dns_resolv.c"' "$PATCHES_MK"
}

@test "libhv working tree has the resolver wired into ResolveAddr" {
    # Any build applies patches; after that the wiring must be live in the tree.
    [ -f "$HSOCKET" ] || skip "libhv submodule not checked out"
    grep -q 'dns_resolv_resolve' "$HSOCKET"
}

@test "guard self-heals a half-applied (orphan-file) state" {
    # Reproduce the EXACT regression against a throwaway tree and prove the
    # fixed guard's heal sequence recovers it.
    command -v git >/dev/null || skip "git not available"
    git -C "$REPO_ROOT/lib/libhv" rev-parse HEAD >/dev/null 2>&1 \
        || skip "libhv submodule not a git repo"

    local work="$BATS_TEST_TMPDIR/libhv"
    mkdir -p "$work/base"
    git -C "$work" init -q
    git -C "$work" config user.email t@t
    git -C "$work" config user.name t

    # Seed the work tree with the PRISTINE (unpatched) hsocket.c from libhv HEAD,
    # so the real patch's context lines match.
    git -C "$REPO_ROOT/lib/libhv" show HEAD:base/hsocket.c > "$work/base/hsocket.c"
    git -C "$work" add base/hsocket.c
    git -C "$work" commit -qm pristine

    # Manufacture the broken state: orphan resolver files + pristine hsocket.c.
    echo "/* orphan from a prior partial apply */" > "$work/base/dns_resolv.c"
    echo "/* orphan from a prior partial apply */" > "$work/base/dns_resolv.h"
    run grep -q 'dns_resolv_resolve' "$work/base/hsocket.c"
    [ "$status" -ne 0 ]   # confirm: starts unwired

    # Heal sequence — mirrors the recipe in mk/patches.mk.
    if ! grep -q 'dns_resolv_resolve' "$work/base/hsocket.c"; then
        rm -f "$work/base/dns_resolv.c" "$work/base/dns_resolv.h"
        git -C "$work" checkout -- base/hsocket.c 2>/dev/null || true
        git -C "$work" apply "$PATCH"
    fi

    # Recovered: wiring present and resolver files recreated by the patch.
    grep -q 'dns_resolv_resolve' "$work/base/hsocket.c"
    [ -f "$work/base/dns_resolv.c" ]
    [ -f "$work/base/dns_resolv.h" ]
}

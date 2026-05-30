#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for download, fetch, and tarball validation functions
# in scripts/lib/installer/release.sh

RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"

    # Reset source guard so module re-sources cleanly
    unset _HELIX_RELEASE_SOURCED
    source "$RELEASE_SH"

    # Pre-set the _has_real_curl cache so mock curl scripts don't need to
    # handle --version.  Tests that exercise the "no curl" path use subshells
    # with restricted PATH and re-source release.sh, so this won't affect them.
    _REAL_CURL=yes

    # Override log_error so validate_tarball output is testable
    # (helpers.bash stubs it as a no-op, but we need the messages)
    log_error() { echo "ERROR: $*"; }
    export -f log_error

    # Isolated test environment
    export TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export CLEANUP_TMP=""

    mkdir -p "$TMP_DIR"
}

# Helper: create a valid gzip of a given size (in KB)
create_valid_gzip() {
    local dest=$1
    local size_kb=${2:-2048}
    dd if=/dev/urandom bs=1024 count="$size_kb" 2>/dev/null | gzip > "$dest"
}

# Helper: build a PATH dir containing ONLY a real python3 (no curl/wget).
# Lets us exercise the python urllib fallback in fetch_url/download_file/
# check_https_capability with no curl/wget to leak through.
make_pybin() {
    local pybin="$BATS_TEST_TMPDIR/pybin"
    mkdir -p "$pybin"
    local real
    real="$(command -v python3 2>/dev/null)" || real="$(command -v python 2>/dev/null)"
    [ -n "$real" ] || return 1
    ln -sf "$real" "$pybin/$(basename "$real")"
    echo "$pybin"
}

# =========================================================================
# fetch_url
# =========================================================================

@test "fetch_url: uses curl when available" {
    mock_command "curl" "hello from curl"
    run fetch_url "http://example.com/test"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from curl"* ]]
}

@test "fetch_url: uses wget when curl not in PATH" {
    local bin="$BATS_TEST_TMPDIR/fetch_bin"
    mkdir -p "$bin"
    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
echo "hello from wget"
MOCK
    chmod +x "$bin/wget"

    # Restricted PATH: mock bin (wget only) + /usr/bin for system utilities.
    # On macOS /usr/bin/curl exists so this test skips there.
    run env PATH="$bin:/usr/bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        fetch_url "http://example.com/test"
    '
    # On macOS, /usr/bin/curl exists, so this test may still find curl.
    # Skip if curl is in /usr/bin (can't isolate on this system)
    if [ -x /usr/bin/curl ]; then
        skip "Cannot isolate from /usr/bin/curl on this system"
    fi
    [ "$status" -eq 0 ]
    [[ "$output" == *"hello from wget"* ]]
}

@test "fetch_url: returns non-zero when neither curl nor wget available" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"
    # Need basic system utils but not curl or wget
    # Symlink only the essentials we need (command is a shell built-in)
    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        fetch_url "http://example.com/test"
    '
    [ "$status" -ne 0 ]
}

@test "fetch_url: returns non-zero when curl fails" {
    mock_command_fail "curl"
    run fetch_url "http://example.com/bad"
    [ "$status" -ne 0 ]
}

@test "fetch_url: handles URL with special characters" {
    mock_command "curl" "ok"
    run fetch_url "http://example.com/path?q=hello+world&lang=en"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "fetch_url: fails gracefully with empty URL" {
    mock_command_fail "curl"
    run fetch_url ""
    [ "$status" -ne 0 ]
}

# =========================================================================
# download_file
# =========================================================================

@test "download_file: succeeds with HTTP 200 and non-empty file" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "binary content" > "$dest"
        echo "200"
    '

    run download_file "http://example.com/release.tar.gz" "$TMP_DIR/test.tar.gz"
    [ "$status" -eq 0 ]
    [ -f "$TMP_DIR/test.tar.gz" ]
}

@test "download_file: fails on HTTP 404" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "Not Found" > "$dest"
        echo "404"
    '

    run download_file "http://example.com/nonexistent" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails on HTTP 500" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && echo "Server Error" > "$dest"
        echo "500"
    '

    run download_file "http://example.com/error" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails when download produces empty file" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$dest" ] && : > "$dest"
        echo "200"
    '

    run download_file "http://example.com/empty" "$TMP_DIR/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: fails when neither curl nor wget available" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        download_file "http://example.com/test" "/tmp/test.tar.gz"
    '
    [ "$status" -ne 0 ]
}

@test "download_file: fails gracefully when dest directory does not exist" {
    mock_command_script "curl" '
        dest=""
        while [ $# -gt 0 ]; do
            case "$1" in
                -o) dest="$2"; shift 2 ;;
                *) shift ;;
            esac
        done
        # Writing to nonexistent dir fails silently
        [ -n "$dest" ] && echo "content" > "$dest" 2>/dev/null
        echo "200"
    '

    run download_file "http://example.com/test" "$BATS_TEST_TMPDIR/nonexistent/dir/test.tar.gz"
    [ "$status" -ne 0 ]
}

@test "download_file: uses wget when curl not in PATH" {
    local bin="$BATS_TEST_TMPDIR/dl_bin"
    mkdir -p "$bin"

    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
dest=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) dest="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$dest" ] && echo "wget content" > "$dest"
exit 0
MOCK
    chmod +x "$bin/wget"

    # Use restricted PATH with only our bin (wget) and system dirs
    # On macOS /usr/bin has curl, so skip if we can't isolate
    if [ -x /usr/bin/curl ]; then
        skip "Cannot isolate from /usr/bin/curl on this system"
    fi

    run env PATH="$bin:/usr/bin:/bin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        download_file 'http://example.com/test' '$TMP_DIR/wget_test.tar.gz'
    "
    [ "$status" -eq 0 ]
}

# =========================================================================
# validate_tarball
# =========================================================================

@test "validate_tarball: accepts valid gzip above 1MB" {
    create_valid_gzip "$TMP_DIR/good.tar.gz" 2048
    run validate_tarball "$TMP_DIR/good.tar.gz" "Test "
    [ "$status" -eq 0 ]
}

@test "validate_tarball: rejects non-gzip file" {
    echo "this is plain text, not a gzip archive" > "$TMP_DIR/fake.tar.gz"
    run validate_tarball "$TMP_DIR/fake.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "validate_tarball: rejects valid gzip that is too small" {
    echo "tiny" | gzip > "$TMP_DIR/tiny.tar.gz"
    run validate_tarball "$TMP_DIR/tiny.tar.gz" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"too small"* ]]
}

@test "validate_tarball: rejects empty file" {
    : > "$TMP_DIR/empty.tar.gz"
    run validate_tarball "$TMP_DIR/empty.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "validate_tarball: rejects nonexistent file" {
    run validate_tarball "$TMP_DIR/does_not_exist.tar.gz" "Test "
    [ "$status" -ne 0 ]
}

@test "validate_tarball: includes context string in error message" {
    echo "not gzip" > "$TMP_DIR/bad.tar.gz"
    run validate_tarball "$TMP_DIR/bad.tar.gz" "Downloaded "
    [ "$status" -ne 0 ]
    [[ "$output" == *"Downloaded "* ]]
}

@test "validate_tarball: rejects binary garbage (not gzip magic)" {
    dd if=/dev/urandom bs=1024 count=2048 of="$TMP_DIR/garbage.tar.gz" 2>/dev/null
    run validate_tarball "$TMP_DIR/garbage.tar.gz" "Test "
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

# =========================================================================
# check_https_capability
# =========================================================================

@test "check_https_capability: returns non-zero with no curl or wget" {
    local bin="$BATS_TEST_TMPDIR/empty_bin"
    mkdir -p "$bin"

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        check_https_capability
    '
    [ "$status" -ne 0 ]
}

@test "check_https_capability: returns non-zero when curl exists but HTTPS fails" {
    local bin="$BATS_TEST_TMPDIR/https_bin"
    mkdir -p "$bin"

    # curl that always fails (simulating no SSL support)
    printf '#!/bin/sh\nexit 1\n' > "$bin/curl"
    chmod +x "$bin/curl"

    # wget whose --help does not mention https, and also fails on https URLs
    cat > "$bin/wget" << 'MOCK'
#!/bin/sh
case "$1" in
    --help) echo "BusyBox wget" ;;
    *) exit 1 ;;
esac
MOCK
    chmod +x "$bin/wget"

    # Need grep for the wget --help check inside check_https_capability
    ln -sf /usr/bin/grep "$bin/grep" 2>/dev/null || true

    run env PATH="$bin" /bin/bash -c '
        source tests/shell/helpers.bash
        unset _HELIX_RELEASE_SOURCED
        source scripts/lib/installer/release.sh
        check_https_capability
    '
    [ "$status" -ne 0 ]
}

# =========================================================================
# python urllib fallback (no curl/wget in PATH)
# =========================================================================

@test "fetch_url: falls back to python when curl and wget absent" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    local f="$BATS_TEST_TMPDIR/fetch_body.txt"
    printf 'py-fallback-fetch-7788\n' > "$f"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        fetch_url 'file://$f'
    "
    [ "$status" -eq 0 ]
    [[ "$output" == *"py-fallback-fetch-7788"* ]]
}

@test "download_file: falls back to python when curl and wget absent" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    local src="$BATS_TEST_TMPDIR/dl_src.bin"
    local dest="$TMP_DIR/dl_py.bin"
    printf 'py-fallback-download-9900\n' > "$src"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        download_file 'file://$src' '$dest'
    "
    [ "$status" -eq 0 ]
    [ -f "$dest" ]
    grep -q "py-fallback-download-9900" "$dest"
}

@test "check_https_capability: returns 0 when only python available" {
    local pybin
    pybin=$(make_pybin) || skip "no python interpreter available"

    run env PATH="$pybin" /bin/bash -c "
        source tests/shell/helpers.bash
        unset _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED _REAL_CURL
        source scripts/lib/installer/common.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        check_https_capability
    "
    [ "$status" -eq 0 ]
}

# =========================================================================
# use_local_tarball
# =========================================================================

@test "use_local_tarball: creates symlink for valid tarball" {
    create_valid_gzip "$BATS_TEST_TMPDIR/local_release.tar.gz" 2048

    run use_local_tarball "$BATS_TEST_TMPDIR/local_release.tar.gz"
    [ "$status" -eq 0 ]
    [ -e "$TMP_DIR/helixscreen.tar.gz" ]
}

@test "use_local_tarball: exits with error for missing file" {
    run use_local_tarball "$BATS_TEST_TMPDIR/no_such_file.tar.gz"
    [ "$status" -ne 0 ]
}

@test "use_local_tarball: exits with error for invalid gzip" {
    echo "not a real tarball" > "$BATS_TEST_TMPDIR/bad_local.tar.gz"
    run use_local_tarball "$BATS_TEST_TMPDIR/bad_local.tar.gz"
    [ "$status" -ne 0 ]
    [[ "$output" == *"not a valid gzip archive"* ]]
}

@test "use_local_tarball: skips symlink when src equals dest" {
    mkdir -p "$TMP_DIR"
    create_valid_gzip "$TMP_DIR/helixscreen.tar.gz" 2048

    run use_local_tarball "$TMP_DIR/helixscreen.tar.gz"
    [ "$status" -eq 0 ]
    # File should still be a regular file, not a symlink to itself
    [ -f "$TMP_DIR/helixscreen.tar.gz" ]
}

@test "use_local_tarball: falls back to copy when symlink fails" {
    create_valid_gzip "$BATS_TEST_TMPDIR/copy_test.tar.gz" 2048

    # Shadow ln with a failing script so ln -sf fails
    mock_command_fail "ln"

    run use_local_tarball "$BATS_TEST_TMPDIR/copy_test.tar.gz"
    [ "$status" -eq 0 ]
    [ -f "$TMP_DIR/helixscreen.tar.gz" ]
}

# =========================================================================
# parse_manifest_version
# =========================================================================

@test "parse_manifest_version: extracts version from valid JSON" {
    result=$(echo '{"version": "0.9.5", "assets": []}' | parse_manifest_version)
    [ "$result" = "0.9.5" ]
}

@test "parse_manifest_version: returns empty for missing version field" {
    result=$(echo '{"tag": "v1.0.0", "notes": "release"}' | parse_manifest_version)
    [ -z "$result" ]
}

@test "parse_manifest_version: returns first match with multiple version fields" {
    result=$(printf '{"version": "1.0.0"}\n{"version": "2.0.0"}\n' | parse_manifest_version)
    [ "$result" = "1.0.0" ]
}

@test "parse_manifest_version: returns empty for empty input" {
    result=$(echo "" | parse_manifest_version)
    [ -z "$result" ]
}

# =========================================================================
# parse_manifest_platform_url
# =========================================================================

MANIFEST_WITH_ASSETS='{
    "version": "0.9.5",
    "assets": {
        "pi": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz",
            "sha256": "abc123"
        },
        "ad5m": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz",
            "sha256": "def456"
        },
        "k1": {
            "url": "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz",
            "sha256": "ghi789"
        }
    }
}'

@test "parse_manifest_platform_url: extracts correct URL for platform" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "ad5m")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-ad5m-v0.9.5.tar.gz" ]
}

@test "parse_manifest_platform_url: returns empty for missing platform" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "windows")
    [ -z "$result" ]
}

@test "parse_manifest_platform_url: returns empty for malformed JSON" {
    result=$(echo 'not json at all {{{' | parse_manifest_platform_url "pi")
    [ -z "$result" ]
}

@test "parse_manifest_platform_url: returns correct URL among multiple platforms" {
    result=$(echo "$MANIFEST_WITH_ASSETS" | parse_manifest_platform_url "k1")
    [ "$result" = "https://releases.helixscreen.org/stable/helixscreen-k1-v0.9.5.tar.gz" ]
}

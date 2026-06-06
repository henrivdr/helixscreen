#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: printer_seed
# Generic, data-driven per-printer settings.json seeding (GitHub #986).
#
# Some printers need known-good defaults written into the install's
# settings.json BEFORE the app starts for the first time (e.g. a verified
# touch-calibration matrix that the first-run wizard can't compute correctly
# on a given panel). The app's runtime preset loader applies the full preset,
# but too late to skip the first-run touch-calibration wizard, which checks
# settings.json /input/calibration/valid at startup. So a small ALLOWLIST of
# preset paths (today: just input.calibration) must be pre-baked into
# settings.json at install time.
#
# Single source of truth (#986): the source of seed data is the runtime preset
# at assets/config/presets/<printer_id>.json (calibration + hardware mappings +
# TODOs). Only the pre-seed allowlist (input.calibration) is baked into
# settings.json before first launch; the rest of the preset (heaters/fans/leds/
# filament_sensors) is applied by the app's runtime preset loader, NOT pre-baked
# into the user's settings.json. The allowlisted subset is deep-merged WITHOUT
# clobbering any keys the user has already set: existing settings always win.
#
# Seeded ids are recorded to ${INSTALL_DIR}/config/.seeded_settings so uninstall
# can be aware of what the installer wrote.
#
# Reads: INSTALL_DIR, SUDO (and HELIX_PRESET_DIR override for tests)
# Calls: file_sudo() from common.sh

# Source guard
[ -n "${_HELIX_PRINTER_SEED_SOURCED:-}" ] && return 0
_HELIX_PRINTER_SEED_SOURCED=1

# Resolve the directory holding the runtime presets (single source of truth).
# Override with HELIX_PRESET_DIR (tests). Otherwise prefer the installed copy
# under ${INSTALL_DIR}/assets/config/presets, then fall back to the in-repo
# source tree (dev installer running from a checkout).
_helix_preset_dir() {
    if [ -n "${HELIX_PRESET_DIR:-}" ]; then
        echo "$HELIX_PRESET_DIR"
        return 0
    fi
    if [ -n "${INSTALL_DIR:-}" ] && [ -d "${INSTALL_DIR}/assets/config/presets" ]; then
        echo "${INSTALL_DIR}/assets/config/presets"
        return 0
    fi
    # Dev checkout: <repo>/assets/config/presets, relative to this module.
    local _self_dir
    _self_dir="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || _self_dir=""
    if [ -n "$_self_dir" ] && [ -d "${_self_dir}/../../../assets/config/presets" ]; then
        (cd "${_self_dir}/../../../assets/config/presets" && pwd)
        return 0
    fi
    echo "${INSTALL_DIR:-/opt/helixscreen}/assets/config/presets"
}

# Record a seeded printer-id to the state file (for uninstall awareness).
# Args: $1 = printer_id
_record_seeded_settings() {
    local printer_id="$1"
    local state_file="${INSTALL_DIR}/config/.seeded_settings"

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    # Don't duplicate.
    if [ -f "$state_file" ] && grep -qF "settings:${printer_id}" "$state_file" 2>/dev/null; then
        return 0
    fi
    echo "settings:${printer_id}" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$state_file" >/dev/null
}

# Pre-bake the install-time allowlist subset of a printer's runtime preset into
# the install's settings.json. Only the allowlisted preset paths (see
# PRESEED_PATHS in the embedded Python) are extracted and deep-merged; the rest
# of the preset is applied by the app's runtime loader. Existing user keys win
# over the seeded subset (the preset is the lower-priority side). Creates
# settings.json from the subset if it is absent or empty.
#
# Graceful by design: a missing preset is a no-op success, and an absent
# python3 logs a warning and skips WITHOUT failing the install.
#
# Args: $1 = printer_id
seed_settings_for_printer() {
    local printer_id="$1"
    [ -n "$printer_id" ] || return 0

    local preset_dir
    preset_dir="$(_helix_preset_dir)"
    local fragment="${preset_dir}/${printer_id}.json"

    # Missing preset → nothing to seed for this printer. Success.
    if [ ! -f "$fragment" ]; then
        return 0
    fi

    # JSON deep-merge requires python3. jq is NOT guaranteed on minimal targets
    # (established #969 fallback pattern), and a deep recursive merge in pure
    # shell is unsafe. If python3 is unavailable, warn and skip — never fail.
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not available; skipping settings seed for ${printer_id}"
        return 0
    fi

    local settings="${INSTALL_DIR}/config/settings.json"

    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    log_info "Seeding settings defaults for ${printer_id}..."

    # Merge into a temp file we own, then move into place via file_sudo so we
    # respect the same privilege model as the rest of the installer.
    local tmp_out="${settings}.seed.$$"

    # Extract the install-time pre-seed allowlist from the preset, strip any
    # "_"-prefixed provenance keys, then deep-merge that subset into the base
    # settings: existing settings (base) take precedence; the subset fills only
    # keys the base does not already define. Recurses into nested objects.
    if SETTINGS_PATH="$settings" FRAGMENT_PATH="$fragment" TMP_OUT="$tmp_out" python3 - <<'PY'
import json, os, sys

settings_path = os.environ["SETTINGS_PATH"]
fragment_path = os.environ["FRAGMENT_PATH"]
tmp_out = os.environ["TMP_OUT"]

# Install-time pre-seed allowlist: the ONLY preset paths baked into the user's
# settings.json BEFORE first launch. The rest of the preset is applied by the
# app's runtime preset loader. input.calibration must be pre-baked because the
# first-run touch-calibration wizard reads settings.json /input/calibration/valid
# at startup, before the runtime preset loader runs.
#
# Adding a path here PRE-BAKES it into settings.json before first launch — only
# do that for settings the app reads before its runtime preset loader applies.
PRESEED_PATHS = [
    ["input", "calibration"],
]

def load(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path) as f:
            data = f.read().strip()
        if not data:
            return {}
        obj = json.loads(data)
        return obj if isinstance(obj, dict) else {}
    except (ValueError, OSError):
        # Malformed existing settings: treat as empty base rather than crash
        # the install. The seeded subset becomes the new content.
        return {}

def strip_underscore(obj):
    """Recursively drop any "_"-prefixed keys (provenance/metadata) so the
    preset's nested documentation (e.g. input.calibration._comment) never leaks
    into the user's settings.json."""
    if isinstance(obj, dict):
        return {k: strip_underscore(v) for k, v in obj.items()
                if not k.startswith("_")}
    if isinstance(obj, list):
        return [strip_underscore(v) for v in obj]
    return obj

def extract_subset(preset, paths):
    """Build a fragment dict containing only the allowlisted paths present in
    the preset, with provenance keys stripped."""
    out = {}
    for path in paths:
        src = preset
        ok = True
        for key in path:
            if isinstance(src, dict) and key in src:
                src = src[key]
            else:
                ok = False
                break
        if not ok:
            continue
        cursor = out
        for key in path[:-1]:
            cursor = cursor.setdefault(key, {})
        cursor[path[-1]] = strip_underscore(src)
    return out

def deep_merge(base, frag, top_level=False):
    """Return base with frag's keys filled in where base lacks them.
    Existing base values always win (fragment is lower priority)."""
    for k, v in frag.items():
        # Defense-in-depth: never copy a top-level "_"-prefixed provenance key
        # into the user's settings.json (the subset is already stripped above).
        if top_level and k.startswith("_"):
            continue
        if k in base and isinstance(base[k], dict) and isinstance(v, dict):
            deep_merge(base[k], v)
        elif k not in base:
            base[k] = v
        # else: base already has a non-dict value → keep it (user wins)
    return base

base = load(settings_path)
preset = load(fragment_path)
frag = extract_subset(preset, PRESEED_PATHS)
merged = deep_merge(base, frag, top_level=True)

with open(tmp_out, "w") as f:
    json.dump(merged, f, indent=2)
    f.write("\n")
PY
    then
        if [ -f "$tmp_out" ]; then
            if $(file_sudo "${INSTALL_DIR}/config") mv "$tmp_out" "$settings" 2>/dev/null; then
                _record_seeded_settings "$printer_id"
                log_success "Seeded settings defaults for ${printer_id}"
            else
                log_warn "Could not write seeded settings.json for ${printer_id}"
                rm -f "$tmp_out" 2>/dev/null || true
            fi
        fi
    else
        log_warn "Failed to merge settings seed for ${printer_id}; leaving settings.json unchanged"
        rm -f "$tmp_out" 2>/dev/null || true
    fi

    return 0
}

# Detect a known printer-model that needs install-time seeding/config.
# Prints a printer-id to stdout (empty if none matched).
#
# Returns "sovol_sv06_ace" when the stock Sovol touchscreen UI binary
# `mksclient` is present at its confirmed install path
# /home/sovol/printer_data/build/mksclient (also globbed as
# /home/*/printer_data/build/mksclient for robustness if the user account
# differs). This is the confirmed fingerprint: published Sovol/R8CEH configs
# show mksclient is a plain binary at that path, run as user `sovol`. Empty
# otherwise.
#
# TODO(#986): the binary path + `sovol` user is now the CONFIRMED fingerprint
# (from published Sovol/R8CEH firmware configs). The only remaining uncertainty
# is the exact /etc/hostname and /proc/device-tree/model strings on the panel,
# which we chose not to fetch — the binary path is a strong, unambiguous signal
# on its own, so hostname matching was dropped to avoid false negatives on
# renamed hosts.
detect_printer_model() {
    # Confirmed signal: the stock Sovol UI binary at its known build path.
    # HELIX_SOVOL_MKSCLIENT lets tests redirect the path under a temp HOME.
    local _bin
    for _bin in \
        "${HELIX_SOVOL_MKSCLIENT:-}" \
        /home/sovol/printer_data/build/mksclient \
        /home/*/printer_data/build/mksclient; do
        [ -n "$_bin" ] || continue
        if [ -f "$_bin" ]; then
            echo "sovol_sv06_ace"
            return 0
        fi
    done

    echo ""
    return 0
}

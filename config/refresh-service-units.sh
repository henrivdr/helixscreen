#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refresh systemd unit files from newly extracted install dir templates.
#
# Called by helixscreen-update.service after Moonraker extracts a new release.
# This script lives in the install dir (not /etc/systemd/system/) so Moonraker's
# extraction always updates it — solving the chicken-and-egg where the installer's
# global sed pass would corrupt @@placeholder@@ patterns embedded directly in the
# systemd unit file.
#
# Reads User/Group from the CURRENTLY installed helixscreen.service before
# overwriting, then templates all @@placeholders@@ in the new copies.
# Also refreshes the watcher units (update.service + update.path) so future
# Moonraker updates pick up any fixes to the watcher mechanism itself.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="$(dirname "$SCRIPT_DIR")"
PDIR="$(dirname "$IDIR")"

DEST="/etc/systemd/system/helixscreen.service"

# Nothing to do if main service isn't installed
[ -f "$DEST" ] || exit 0

# Wait for Moonraker's extraction to settle before we touch INSTALL_DIR.
#
# helixscreen-update.path fires PathChanged on release_info.json as soon as
# Moonraker recreates it during zip extraction; the update.service then waits
# 10s and calls us.  On slow MIPS flash (AD5X / K1) Moonraker may still be
# extracting at that point, so refresh_config_symlinks would race against the
# extraction and leave the install dir in a state Moonraker can't finish
# cleaning up — producing the "ENOTEMPTY: /srv/helixscreen" toast in Mainsail
# and a binary-missing crash loop afterward.
#
# Heuristic: binary + release_info.json both present and the binary's size
# stable across two polls 2s apart.  Times out after 90s and proceeds anyway
# (a half-broken refresh is still better than not refreshing at all — e.g.
# during recovery when the binary is missing entirely).
wait_for_install_stable() {
    local binary="${IDIR}/bin/helix-screen"
    local release_info="${IDIR}/release_info.json"
    local budget=90 elapsed=0 prev_size=-1 stable=0 size

    while [ "$elapsed" -lt "$budget" ]; do
        if [ -x "$binary" ] && [ -f "$release_info" ]; then
            size=$(stat -c%s "$binary" 2>/dev/null || echo -1)
            if [ "$size" -gt 0 ] && [ "$size" = "$prev_size" ]; then
                stable=$((stable + 1))
                [ "$stable" -ge 2 ] && return 0
            else
                stable=0
                prev_size="$size"
            fi
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "refresh-service-units: install dir not stable after ${budget}s — proceeding" >&2
    return 1
}

wait_for_install_stable || true

# Read current identity from the installed service file BEFORE overwriting
USER_VAL="$(grep "^User=" "$DEST" | cut -d= -f2)"
GROUP_VAL="$(grep "^Group=" "$DEST" | cut -d= -f2)"

template_unit() {
    local src="$1" dest="$2"
    cp "$src" "$dest" || return 1
    sed -i \
        -e "s|@@HELIX_USER@@|${USER_VAL:-root}|g" \
        -e "s|@@HELIX_GROUP@@|${GROUP_VAL:-root}|g" \
        -e "s|@@INSTALL_DIR@@|${IDIR}|g" \
        -e "s|@@INSTALL_PARENT@@|${PDIR}|g" \
        "$dest"
}

# Refresh main service
SRC="${IDIR}/config/helixscreen.service"
[ -f "$SRC" ] && template_unit "$SRC" "$DEST"

# Refresh watcher units (this service + path unit)
for F in helixscreen-update.service helixscreen-update.path; do
    FSRC="${IDIR}/config/${F}"
    FDEST="/etc/systemd/system/${F}"
    [ -f "$FSRC" ] && template_unit "$FSRC" "$FDEST"
done

systemctl daemon-reload

# --- Restore config symlinks after Moonraker update ---
#
# Moonraker type:web does shutil.rmtree(INSTALL_DIR) then extracts a fresh ZIP.
# This destroys the symlinks from INSTALL_DIR/config/ → printer_data/config/helixscreen/
# and replaces them with default config files from the release ZIP.  The user's real
# config survives in printer_data (outside the managed path) but is orphaned.
#
# Re-create symlinks so the app reads the user's config instead of defaults.
# Runs as root (inherited from the calling service unit), so no sudo needed.

HELIX_USER_CONFIG_FILES="settings.json helixscreen.env .disabled_services tool_spools.json"

restore_config_symlinks() {
    local install_config="${IDIR}/config"
    [ -d "$install_config" ] || return 0

    # Discover printer_data/config/helixscreen/ from the service user's home.
    # Try the service user's home first, then scan common locations.
    local pd_helix=""
    local user_home=""

    if [ -n "$USER_VAL" ] && [ "$USER_VAL" != "root" ]; then
        user_home=$(eval echo "~${USER_VAL}" 2>/dev/null || echo "")
    fi

    # Check user home first, then common Klipper user homes, then /root
    for candidate in \
        "${user_home}/printer_data/config/helixscreen" \
        /home/pi/printer_data/config/helixscreen \
        /home/biqu/printer_data/config/helixscreen \
        /home/mks/printer_data/config/helixscreen \
        /root/printer_data/config/helixscreen \
        /usr/data/printer_data/config/helixscreen; do
        [ -z "$candidate" ] && continue
        if [ -d "$candidate" ]; then
            pd_helix="$candidate"
            break
        fi
    done

    [ -n "$pd_helix" ] || return 0

    for file in $HELIX_USER_CONFIG_FILES; do
        local pd_file="${pd_helix}/${file}"
        local install_file="${install_config}/${file}"

        # Already a correct symlink — nothing to do
        if [ -L "$install_file" ]; then
            local target
            target=$(readlink "$install_file" 2>/dev/null || echo "")
            [ "$target" = "$pd_file" ] && continue
            rm -f "$install_file"
        fi

        # User's config exists in printer_data — replace the default with a symlink
        if [ -f "$pd_file" ]; then
            # Remove the default file shipped in the ZIP
            rm -f "$install_file" 2>/dev/null
            ln -s "$pd_file" "$install_file" 2>/dev/null || true
        fi
    done
}

restore_config_symlinks

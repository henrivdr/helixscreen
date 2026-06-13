#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Code lint tests: enforce architectural rules on the codebase.

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# --- No _for_testing methods in production code ---
# Test-only methods belong in test files via friend class TestAccess pattern.
# See commit removing these for the migration pattern.
#
# *_mock.h files are explicitly excluded: mocks ARE test infrastructure (the
# whole class exists only for tests), so a `_for_testing` setter on a mock
# carries no risk of shipping test code to users — the mock itself is gated
# by HELIX_ENABLE_MOCKS and never enters production builds.

@test "no _for_testing methods declared in headers" {
    run grep -rn '_for_testing' include/ --include='*.h' --exclude='*_mock.h'
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

@test "no _for_testing methods defined in source files" {
    run grep -rn '_for_testing' src/ --include='*.cpp'
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

# --- Migrated temperature VIEW files must route sends through the controller ---
# ui_overlay_temp_graph.cpp and ui_panel_controls.cpp were migrated to delegate
# temperature commands to helix::TemperatureController. They must NOT call the
# raw send API directly again — that would reintroduce the duplication the
# refactor removed. A direct send is either `api_->set_temperature(` or any
# `->set_temperature(` whose receiver is not a `controller`.

@test "migrated temp view files do not call the raw set_temperature send API" {
    local files="src/ui/ui_overlay_temp_graph.cpp src/ui/ui_panel_controls.cpp src/system/post_op_cooldown_manager.cpp src/ui/panel_widgets/preheat_widget.cpp src/ui/ui_panel_bed_mesh.cpp src/ui/ui_panel_filament.cpp src/ui/temperature_service.cpp src/ui/ui_ams_sidebar.cpp"

    # Direct API send on the cached MoonrakerAPI pointer.
    run grep -n 'api_->set_temperature' $files
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found

    # Any ->set_temperature( call whose receiver is not `controller`. The
    # controller's own ->set_temperature() is the sanctioned path, so exclude it.
    run bash -c "grep -nE '\->set_temperature\(' $files | grep -v 'controller'"
    [ "$status" -ne 0 ]  # non-zero == no disallowed direct send found
}

# --- Chamber temp_display must use the maintain-aware effective target ---
# The raw `chamber_target` subject is the heater target only — it reads 0 during
# M141 "maintain" (cooling-ceiling) mode, so a display bound to it shows "—/Off"
# while the chamber is actually holding a setpoint. All chamber temp_display
# instances must bind to `chamber_effective_target` (+ `chamber_mode`), never the
# raw subject. (Drip-fixed missed-spot class; see chamber M141 routing work.)

@test "no temp_display binds chamber target to the raw chamber_target subject" {
    run grep -rn 'bind_target="chamber_target"' ui_xml/
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found
}

# --- Temperature "decidegrees" misnomer must not creep back ---
# Subject temperatures are stored as degrees × 10 = DECIdegrees (1 unit = 0.1°C).
# The codebase was historically (and wrongly) calling these "centidegrees" — a
# centidegree would be degrees × 100. The Phase-1 rename swept the misnomer out;
# this gate keeps it from reappearing in code or developer docs.
#
# Excluded paths are legitimately historical or out-of-scope:
#   CHANGELOG.md          - records the old name as part of release history
#   .claude/ .claude-recall/ - assistant memory/lesson logs, not shipped code
#   docs/superpowers/     - archived planning docs
#   src/generated/        - generated code (regenerated from templates)
#   lib/                  - vendored submodules (LVGL, libhv, etc.)
#   */translations/       - translation catalogs (mirror upstream wording)

@test "no 'centidegree' misnomer in code or docs" {
    run bash -c "git grep -iIln 'centidegree' -- \
        ':!CHANGELOG.md' \
        ':(glob)!.claude/**' \
        ':(glob)!.claude-recall/**' \
        ':(glob)!docs/superpowers/**' \
        ':(glob)!src/generated/**' \
        ':(glob)!lib/**' \
        ':(glob)!translations/**' \
        ':(glob)!ui_xml/translations/**' \
        ':(glob)!scripts/translations/**'"
    # git grep exits 0 when it finds matches, 1 when it finds none.
    [ "$status" -ne 0 ]  # non-zero == no misnomer found
}

# --- Temperature subject conversions must route through the unit helpers ---
# Subjects store decidegrees; converting to/from degrees inline (`x / 10`,
# `x * 10`) bypasses helix::units / helix::ui::temperature and silently risks a
# truncation/rounding mismatch (int trunc vs float). These files were migrated to
# the helpers (deci_to_degrees / deci_to_degrees_f / degrees_to_deci /
# to_decidegrees / from_decidegrees); they must not reintroduce a raw multiply or
# divide by 10 on a temperature-named value.
#
# The regex matches a temperature identifier (target/temp/deci/nozzle/bed/chamber/
# heater) or a bare keypad `value`, optionally closing a paren, then `* 10` or
# `/ 10` — but NOT `* 100` / `/ 100` (the (\.0?f?)?([^0-9.]|$) tail rejects a
# trailing digit so centimm/centi conversions are untouched). `//` comments are
# stripped first so the "value * 10" explanatory comments don't trip the gate.

@test "migrated temp files do not convert decidegrees inline (use unit helpers)" {
    local files="src/print/print_start_collector.cpp \
        src/api/moonraker_api_controls.cpp \
        src/api/moonraker_discovery_sequence.cpp \
        src/printer/ams_backend_ad5x_ifs.cpp \
        src/printer/ams_backend_cfs.cpp \
        src/system/telemetry_manager.cpp \
        src/ui/panel_widgets/nozzle_temps_widget.cpp \
        src/ui/ui_ams_sidebar.cpp \
        src/ui/temperature_service.cpp \
        src/ui/ui_overlay_temp_graph.cpp \
        src/ui/ui_panel_bed_mesh.cpp \
        src/ui/ui_panel_calibration_pid.cpp \
        src/ui/ui_panel_controls.cpp \
        src/ui/ui_panel_filament.cpp \
        src/ui/ui_print_preparation_manager.cpp \
        src/ui/ui_temp_display.cpp"
    local pat='(target|temp|deci|nozzle|bed|chamber|heater|value)[A-Za-z_]*(\s*\))?\s*[*/]\s*10(\.0?f?)?([^0-9.]|$)'
    run bash -c "sed -E 's@//.*@@' $files | grep -nE '$pat'"
    [ "$status" -ne 0 ]  # non-zero == no inline decidegree conversion found
}

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
# `->set_temperature(` whose receiver is not a `controller`. (temperature_service.cpp
# legitimately retains other send paths and is intentionally NOT covered here.)

@test "migrated temp view files do not call the raw set_temperature send API" {
    local files="src/ui/ui_overlay_temp_graph.cpp src/ui/ui_panel_controls.cpp src/system/post_op_cooldown_manager.cpp src/ui/panel_widgets/preheat_widget.cpp"

    # Direct API send on the cached MoonrakerAPI pointer.
    run grep -n 'api_->set_temperature' $files
    [ "$status" -eq 1 ]  # grep returns 1 when no matches found

    # Any ->set_temperature( call whose receiver is not `controller`. The
    # controller's own ->set_temperature() is the sanctioned path, so exclude it.
    run bash -c "grep -nE '\->set_temperature\(' $files | grep -v 'controller'"
    [ "$status" -ne 0 ]  # non-zero == no disallowed direct send found
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace helix {

/// Returns true when running on Android (compile-time on real builds, overridable for tests)
bool is_android_platform();

/// Test helper: override the platform check. Pass -1 to reset to compile-time default.
void set_platform_override(int override_value);

/// Log platform info (kernel, arch, hostname, memory) at INFO level
void log_platform_info();

/// Short human-readable host arch line for the About screen and debug bundles.
/// Format: "<kernel-arch> · <N>-bit userspace" — surfaces the common
/// "aarch64 kernel + 32-bit userspace" Pi configuration without coercing the
/// user to migrate.
std::string host_arch_string();

} // namespace helix

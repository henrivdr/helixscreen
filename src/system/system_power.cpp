// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system_power.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>

#ifdef HELIX_HAS_SYSTEMD
#include <systemd/sd-bus.h>
#endif

namespace helix {
namespace {

#ifdef HELIX_HAS_SYSTEMD
bool logind_call(const char* method) {
    sd_bus* bus = nullptr;
    const int open_rc = sd_bus_open_system(&bus);
    if (open_rc < 0 || !bus) {
        spdlog::warn("[SystemPower] sd_bus_open_system failed: {}", std::strerror(-open_rc));
        return false;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager", method, &err, &reply, "b",
                                     0 /* interactive=false */);
    const bool ok = r >= 0;
    if (!ok) {
        spdlog::warn("[SystemPower] logind {} failed: {} ({})", method,
                     err.message ? err.message : "no message", err.name ? err.name : "");
    }
    sd_bus_error_free(&err);
    if (reply)
        sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return ok;
}
#endif

bool exec_fallback(const char* desc, const char* cmd) {
    spdlog::info("[SystemPower] fallback: {}", desc);
    const int rc = std::system(cmd);
    if (rc == -1) {
        spdlog::warn("[SystemPower] {} fork/exec failed", desc);
        return false;
    }
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

// Non-systemd hosts (OpenWrt/procd K2, K1C, AD5M, SonicPad…) don't have
// systemctl. They do have busybox /sbin/reboot and /sbin/poweroff via
// procd's sysinit hook (`::shutdown:/etc/init.d/rcS K shutdown`).
bool busybox_fallback(const char* path) {
    return exec_fallback(path, path);
}

} // namespace

bool SystemPower::reboot_local() {
    spdlog::info("[SystemPower] reboot_local");
#ifdef HELIX_HAS_SYSTEMD
    if (logind_call("Reboot"))
        return true;
#endif
    if (exec_fallback("systemctl reboot", "systemctl reboot"))
        return true;
    return busybox_fallback("/sbin/reboot");
}

bool SystemPower::shutdown_local() {
    spdlog::info("[SystemPower] shutdown_local");
#ifdef HELIX_HAS_SYSTEMD
    if (logind_call("PowerOff"))
        return true;
#endif
    if (exec_fallback("systemctl poweroff", "systemctl poweroff"))
        return true;
    return busybox_fallback("/sbin/poweroff");
}

} // namespace helix

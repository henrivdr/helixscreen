// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/log_path_probe.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace helix::system {

LogPathProbeResult probe_log_path_writable(const std::string& path, std::uint64_t min_free_bytes) {
    LogPathProbeResult result{false, {}};

    const std::size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);

    struct statvfs vfs {};
    if (::statvfs(dir.c_str(), &vfs) == 0) {
        const std::uint64_t avail =
            static_cast<std::uint64_t>(vfs.f_bavail) * static_cast<std::uint64_t>(vfs.f_frsize);
        if (avail < min_free_bytes) {
            result.error = "only " + std::to_string(avail) + " bytes free on " + dir + " (need ≥ " +
                           std::to_string(min_free_bytes) + ")";
            return result;
        }
    }
    // statvfs() failure: fall through — open() below surfaces the real errno.

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) {
        result.error = std::strerror(errno);
        return result;
    }

    static constexpr char probe[] = "# helixscreen install log\n";
    const auto probe_len = static_cast<ssize_t>(sizeof(probe) - 1);
    const ssize_t n = ::write(fd, probe, probe_len);
    const int write_errno = errno;
    ::close(fd);
    if (n != probe_len) {
        ::unlink(path.c_str());
        result.error = (n < 0) ? std::strerror(write_errno) : "short write";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace helix::system

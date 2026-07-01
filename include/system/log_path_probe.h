// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace helix::system {

struct LogPathProbeResult {
    bool ok;
    std::string error; // populated when ok == false
};

/**
 * Verify that `path` can hold a multi-KB log file.
 *
 * Two-stage check, in order:
 *   1. statvfs() the parent directory and require ≥ min_free_bytes available.
 *      Catches the nearly-full-tmpfs case where open(O_CREAT|O_TRUNC) succeeds
 *      (inode allocation is metadata-only, O_TRUNC frees blocks) but later
 *      writes fail with ENOSPC.  If statvfs() itself fails, this stage is
 *      skipped — the open() below will surface the real errno.
 *   2. open(O_WRONLY|O_CREAT|O_TRUNC) and write a sentinel byte sequence.
 *      Catches permissions, quota, and other failure modes statvfs() can't see.
 *      On failure the path is unlink()'d so no zero-byte stub is left behind.
 *
 * On success the file at `path` exists and contains the probe sentinel; the
 * caller is expected to reopen with O_TRUNC and write its own content.
 */
LogPathProbeResult probe_log_path_writable(const std::string& path, std::uint64_t min_free_bytes);

} // namespace helix::system

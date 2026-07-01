// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __ANDROID__

#include <string>
#include <utility>

namespace helix::android {

/// HTTP(S) GET via Android's Java HttpURLConnection (JNI bridge).
/// libhv is built without SSL on Android (no NDK OpenSSL), so we route
/// update-check traffic through the platform TLS stack instead.
///
/// Returns {status_code, body_or_error}. status_code == 0 means the
/// request never reached the server (network, DNS, JNI, or TLS failure)
/// and the second element carries a short error message instead of a body.
std::pair<int, std::string> https_get(const std::string& url, const std::string& user_agent,
                                      const std::string& accept, int timeout_sec);

} // namespace helix::android

#endif // __ANDROID__

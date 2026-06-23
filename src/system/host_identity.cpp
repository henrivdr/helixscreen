// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_identity.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <ifaddrs.h>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace helix {
namespace {

std::mutex g_cache_mutex;
std::unordered_map<std::string, bool> g_cache;

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_loopback_literal(std::string_view host) {
    if (host.empty())
        return true;
    const std::string h = to_lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "::1";
}

bool matches_own_hostname(std::string_view host) {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) != 0)
        return false;
    return to_lower(host) == to_lower(buf);
}

bool matches_local_interface_ip(std::string_view host) {
    in_addr v4{};
    in6_addr v6{};
    const std::string h(host);
    const bool is_v4 = inet_pton(AF_INET, h.c_str(), &v4) == 1;
    const bool is_v6 = !is_v4 && inet_pton(AF_INET6, h.c_str(), &v6) == 1;
    if (!is_v4 && !is_v6)
        return false;

    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0)
        return false;

    bool found = false;
    for (auto* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr)
            continue;
        if (is_v4 && p->ifa_addr->sa_family == AF_INET) {
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            if (sin->sin_addr.s_addr == v4.s_addr) {
                found = true;
                break;
            }
        } else if (is_v6 && p->ifa_addr->sa_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(p->ifa_addr);
            if (std::memcmp(&sin6->sin6_addr, &v6, sizeof(v6)) == 0) {
                found = true;
                break;
            }
        }
    }
    freeifaddrs(ifap);
    return found;
}

} // namespace

bool is_moonraker_on_same_host(std::string_view host) {
    std::string key(host);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end())
            return it->second;
    }

    const bool result =
        is_loopback_literal(host) || matches_own_hostname(host) || matches_local_interface_ip(host);

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.emplace(std::move(key), result);
    return result;
}

void invalidate_host_identity_cache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.clear();
}

} // namespace helix

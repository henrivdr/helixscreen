// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

/**
 * @brief Validate IP address or hostname format
 *
 * Accepts:
 * - Valid IPv4 addresses (e.g., "192.168.1.1")
 * - Valid hostnames (e.g., "printer.local", "my-printer")
 *
 * @param host IP address or hostname string
 * @return true if valid, false otherwise
 */
bool is_valid_ip_or_hostname(const std::string& host);

/**
 * @brief Validate port number
 *
 * Accepts port numbers in range 1-65535 (numeric only)
 *
 * @param port_str Port number as string
 * @return true if valid, false otherwise
 */
bool is_valid_port(const std::string& port_str);

/**
 * @brief Strip non-numeric characters from a string
 *
 * Useful for sanitizing port input that may contain stray characters.
 *
 * @param str Input string (may be nullptr)
 * @return String containing only digit characters
 */
std::string sanitize_port(const char* str);
std::string sanitize_port(const std::string& str);

/**
 * @brief Resolve the Moonraker host to pre-fill in the setup wizard
 *
 * Config::get returns a stored value verbatim when the key exists, so a persisted
 * empty host — written by a factory reset or a --wizard re-run — comes back as ""
 * rather than falling through to a default. Treat an empty (or whitespace-only)
 * stored host as "not configured" and fall back to the localhost default, so the
 * wizard pre-fills 127.0.0.1 (the common same-host setup) instead of a blank field.
 * On Android, Moonraker is always remote, so the default is empty.
 *
 * @param stored_host The host string read from config (may be empty)
 * @param is_android  Whether running on the Android platform
 * @return stored_host if it is non-blank, else "127.0.0.1" (non-Android) or "" (Android)
 */
std::string resolve_moonraker_host_default(const std::string& stored_host, bool is_android);

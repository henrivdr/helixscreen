// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.h"

namespace helix {

// Test-only accessor for Config private/protected members. Replaces the
// per-fixture `friend class` declarations that used to live in config.h
// (the friend-TestAccess pattern; see tests/test_helpers/*_test_access.h).
class ConfigTestAccess {
  public:
    static json& data(Config& c) {
        return c.data;
    }
    static std::string& path(Config& c) {
        return c.path;
    }
    static std::string& active_printer_id(Config& c) {
        return c.active_printer_id_;
    }
    // Swap the Config singleton pointer (used by the change-host fixture).
    static Config*& instance_ref() {
        return Config::instance;
    }
};

// Shared replacement for the duplicated per-fixture setup_printer_data helper.
inline void setup_printer_data(Config& config, const json& printer_data) {
    ConfigTestAccess::data(config) = {{"config_version", 3},
                                      {"active_printer_id", "default"},
                                      {"printers", {{"default", printer_data}}}};
    ConfigTestAccess::active_printer_id(config) = "default";
}

} // namespace helix

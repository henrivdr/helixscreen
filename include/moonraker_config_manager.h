// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace helix {
class MoonrakerConfigManager {
  public:
    static bool has_section(const std::string& content, const std::string& section_name);
    static std::string add_section(const std::string& content, const std::string& section_name,
                                   const std::vector<std::pair<std::string, std::string>>& entries,
                                   const std::string& comment = "");
    static std::string remove_section(const std::string& content, const std::string& section_name);
    static bool has_include_line(const std::string& moonraker_content);
    static std::string add_include_line(const std::string& moonraker_content);
    static std::string get_section_value(const std::string& content,
                                         const std::string& section_name, const std::string& key);
};
} // namespace helix

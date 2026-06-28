// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_config_manager.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace helix {

// Trim leading and trailing whitespace (spaces, tabs, carriage returns)
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool MoonrakerConfigManager::has_section(const std::string& content,
                                         const std::string& section_name) {
    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        if (t == target)
            return true;
    }
    return false;
}

std::string
MoonrakerConfigManager::add_section(const std::string& content, const std::string& section_name,
                                    const std::vector<std::pair<std::string, std::string>>& entries,
                                    const std::string& comment) {
    if (has_section(content, section_name))
        return content;

    std::string result = content;
    // Ensure content ends with a newline before appending
    if (!result.empty() && result.back() != '\n')
        result += '\n';

    result += '\n';
    if (!comment.empty())
        result += "# " + comment + "\n";
    result += "[" + section_name + "]\n";
    for (const auto& [key, value] : entries) {
        result += key + ": " + value + "\n";
    }
    return result;
}

std::string MoonrakerConfigManager::remove_section(const std::string& content,
                                                   const std::string& section_name) {
    if (!has_section(content, section_name))
        return content;

    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;

    // Lines before target section (excluding its preceding comment block)
    std::vector<std::string> result_lines;
    // Pending lines that may be a comment block just before the target section
    std::vector<std::string> pending_comment;
    bool in_target = false;

    while (std::getline(stream, line)) {
        std::string t = trim(line);

        if (in_target) {
            // Skip lines until the next section header
            if (!t.empty() && t[0] == '[') {
                in_target = false;
                // Start collecting this new section
                pending_comment.clear();
                result_lines.push_back(line);
            }
            // else: skip this line (it belongs to the removed section)
            continue;
        }

        // Detect target section header
        if (t == target) {
            in_target = true;
            // Discard any pending comment block that preceded this section
            pending_comment.clear();
            continue;
        }

        // Track comment lines as potentially belonging to the next section
        if (t.empty() || t[0] == '#') {
            pending_comment.push_back(line);
        } else {
            // Non-comment, non-target line: flush pending comments into result
            for (const auto& cl : pending_comment)
                result_lines.push_back(cl);
            pending_comment.clear();
            result_lines.push_back(line);
        }
    }

    // If we never entered the target, just flush pending (shouldn't happen due to has_section
    // check)
    if (!in_target) {
        for (const auto& cl : pending_comment)
            result_lines.push_back(cl);
    }
    // If we ended inside the target, discard pending_comment (already cleared when entering target)

    // Build result string, stripping trailing blank lines
    while (!result_lines.empty() && trim(result_lines.back()).empty()) {
        result_lines.pop_back();
    }

    std::string result;
    for (const auto& l : result_lines) {
        result += l + "\n";
    }
    return result;
}

bool MoonrakerConfigManager::has_include_line(const std::string& moonraker_content) {
    return has_section(moonraker_content, "include helixscreen.conf");
}

std::string MoonrakerConfigManager::add_include_line(const std::string& moonraker_content) {
    if (has_include_line(moonraker_content))
        return moonraker_content;

    const std::string include_block = "[include helixscreen.conf]\n\n";

    // Find the first non-comment section header and insert before it
    std::istringstream stream(moonraker_content);
    std::string line;
    size_t pos = 0;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            // Insert before this section header
            std::string result = moonraker_content.substr(0, pos);
            result += include_block;
            result += moonraker_content.substr(pos);
            return result;
        }
        pos += line.size() + 1; // +1 for '\n'
    }

    // No section found — append
    std::string result = moonraker_content;
    if (!result.empty() && result.back() != '\n')
        result += '\n';
    result += include_block;
    return result;
}

std::string MoonrakerConfigManager::get_section_value(const std::string& content,
                                                      const std::string& section_name,
                                                      const std::string& key) {
    const std::string target_section = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;
    bool in_section = false;

    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;

        if (t[0] == '[') {
            in_section = (t == target_section);
            continue;
        }

        if (!in_section)
            continue;

        // Parse "key : value" with optional whitespace
        size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        std::string k = trim(t.substr(0, colon));
        if (k != key)
            continue;
        return trim(t.substr(colon + 1));
    }
    return "";
}

} // namespace helix

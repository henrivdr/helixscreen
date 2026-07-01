// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>
#include <vector>
namespace helix {

struct GcodeLineReplacement {
    int line_number;      // 1-based
    std::string new_line; // replacement text (no trailing newline)
};

class GcodeToolRemapper {
  public:
    // remap: logical tool index -> physical head index.
    // Rewrites all three command families. Each line is mapped from its ORIGINAL
    // index in a single pass (collision-safe for swaps). Lines not matching any
    // remap key are returned unchanged.
    static std::string apply_to_string(const std::string& gcode, const std::map<int, int>& remap);

    // Same logic but returns only the CHANGED lines as (1-based line_number, new text),
    // for feeding the streaming GCodeFileModifier in the production path (Task 12).
    static std::vector<GcodeLineReplacement>
    build_line_replacements(const std::string& gcode, const std::map<int, int>& remap);
};

} // namespace helix

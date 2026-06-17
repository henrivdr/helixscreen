// SPDX-License-Identifier: GPL-3.0-or-later
#include "error_classify.h"

#include "gcode_error_router.h"  // clean_error_text

#include <cctype>

namespace helix::error_classify {

namespace {

ErrorSource source_for_code(const std::string& code) {
    if (code.rfind("key", 0) == 0) return ErrorSource::CFS;  // Creality CFS codes
    return ErrorSource::GENERIC;
}

bool is_error_prefix(const std::string& line) {
    if (line.size() >= 2 && line[0] == '!' && line[1] == '!') return true;
    if (line.size() >= 6) {
        std::string p = line.substr(0, 5);
        for (auto& c : p) c = static_cast<char>(std::tolower(c));
        if (p == "error" && line[5] == ':') return true;
    }
    return false;
}

}  // namespace

std::optional<ErrorEvent> classify(const std::string& raw_line, const ClassifyContext& ctx) {
    if (!is_error_prefix(raw_line)) return std::nullopt;

    ErrorEvent e;
    const bool is_bang = raw_line.size() >= 2 && raw_line[0] == '!' && raw_line[1] == '!';

    std::string text;
    if (is_bang) {
        text = (raw_line.size() >= 3 && raw_line[2] == ' ') ? raw_line.substr(3)
                                                            : raw_line.substr(2);
    } else {  // "Error:"
        text = (raw_line.size() >= 7 && raw_line[6] == ' ') ? raw_line.substr(7)
                                                            : raw_line.substr(6);
    }

    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);
    e.detail = text;
    e.code = code;

    if (!is_bang) {  // "Error:" command error
        e.source = ErrorSource::KLIPPER;
        e.severity = ErrorSeverity::WARNING;
    } else if (!code.empty()) {
        e.source = source_for_code(code);
        if (code.rfind("key8", 0) == 0) {
            e.severity = ErrorSeverity::CRITICAL;
            if (code == "key840") {
                e.recovery_actions.push_back(
                    {"Reset CFS", "BOX_ERROR_CLEAR", "error_classify::key840_reset"});
            }
        } else if (code == "key298") {
            e.severity = ErrorSeverity::WARNING;
            e.recovery_actions.push_back({"Recover", "", "error_classify::key298_recover"});
        } else {
            e.severity = ErrorSeverity::WARNING;
        }
    } else {  // uncoded `!!` — AFC-jam case
        e.source = ErrorSource::GENERIC;
        e.severity = (ctx.is_paused || ctx.is_printing) ? ErrorSeverity::CRITICAL
                                                        : ErrorSeverity::WARNING;
    }

    // Sticky is uniform across sources: any CRITICAL stays on screen until the
    // user dismisses it; WARNING/INFO auto-dismiss. Computing it once here keeps
    // coded (key8xx) and uncoded CRITICAL paths consistent for the L1 presenter.
    e.sticky = (e.severity == ErrorSeverity::CRITICAL);
    return e;
}

}  // namespace helix::error_classify

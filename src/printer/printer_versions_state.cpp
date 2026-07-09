// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_versions_state.cpp
 * @brief Software version state management extracted from PrinterState
 *
 * Manages Klipper and Moonraker version subjects for UI display in the
 * Settings panel About section.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_versions_state.h"

#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix {

namespace {

// Some Moonraker forks (notably QIDI's on the Q2 / Max 4) report a literal "?"
// placeholder for versions instead of omitting the field, and the discovery
// parse layer substitutes "unknown" when a key is missing. Neither is meaningful
// to show, so collapse them (and empty strings) to a translated "Unknown" label.
std::string display_version(const std::string& version) {
    std::string trimmed = version;
    trimmed.erase(trimmed.begin(),
                  std::find_if(trimmed.begin(), trimmed.end(),
                               [](unsigned char c) { return !std::isspace(c); }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(),
                               [](unsigned char c) { return !std::isspace(c); })
                      .base(),
                  trimmed.end());

    std::string lowered = trimmed;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (trimmed.empty() || trimmed == "?" || lowered == "unknown") {
        return lv_tr("Unknown");
    }
    return trimmed;
}

} // namespace

void PrinterVersionsState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterVersionsState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterVersionsState] Initializing subjects (register_xml={})", register_xml);

    // Initialize string subjects with em dash default
    INIT_SUBJECT_STRING(klipper_version, "—", subjects_, register_xml);
    INIT_SUBJECT_STRING(moonraker_version, "—", subjects_, register_xml);
    INIT_SUBJECT_STRING(os_version, "—", subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterVersionsState] Subjects initialized successfully");
}

void PrinterVersionsState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterVersionsState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterVersionsState::set_klipper_version_internal(const std::string& version) {
    lv_subject_copy_string(&klipper_version_, display_version(version).c_str());
    spdlog::debug("[PrinterVersionsState] Klipper version set: {}", version);
}

void PrinterVersionsState::set_moonraker_version_internal(const std::string& version) {
    lv_subject_copy_string(&moonraker_version_, display_version(version).c_str());
    spdlog::debug("[PrinterVersionsState] Moonraker version set: {}", version);
}

void PrinterVersionsState::set_os_version_internal(const std::string& version) {
    lv_subject_copy_string(&os_version_, display_version(version).c_str());
    spdlog::debug("[PrinterVersionsState] OS version set: {}", version);
}

} // namespace helix

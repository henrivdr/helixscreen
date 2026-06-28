// SPDX-License-Identifier: GPL-3.0-or-later
#include "hardware_role_registry.h"

#include "config.h"
#include "printer_hardware.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// --- is_candidate predicates: applied by Tier 1a/1b only (NOT Tier 0) ---
// These gate heuristic/canonical picks so a fallback guess can't pick a wrong-category object
// (e.g. guess_part_cooling_fan() falling back to fans_[0] which is a controller_fan).

// Part cooling: anything that is NOT an auto-controlled fan type.
bool is_part_fan_candidate(const std::string& o) {
    return o.rfind("heater_fan ", 0) != 0 && o.rfind("controller_fan ", 0) != 0 &&
           o.rfind("temperature_fan ", 0) != 0;
}
// Hotend / heatbreak cooling fan.
bool is_hotend_fan_candidate(const std::string& o) {
    return o.rfind("heater_fan ", 0) == 0 || o.find("hotend") != std::string::npos ||
           o.find("heat") != std::string::npos || o.find("heatbreak") != std::string::npos;
}
// Chamber circulation / filter fan.
bool is_chamber_fan_candidate(const std::string& o) {
    return o.find("chamber") != std::string::npos || o.find("nevermore") != std::string::npos ||
           o.find("filter") != std::string::npos || o.rfind("temperature_fan ", 0) == 0;
}
// Exhaust / vent fan.
bool is_exhaust_fan_candidate(const std::string& o) {
    return o.find("exhaust") != std::string::npos || o.find("vent") != std::string::npos ||
           o.find("external") != std::string::npos;
}
bool is_bed_heater_candidate(const std::string& o) {
    return o.find("bed") != std::string::npos;
}
bool is_hotend_heater_candidate(const std::string& o) {
    return o == "extruder" || o.rfind("extruder", 0) == 0 ||
           o.find("hotend") != std::string::npos || o == "e0";
}

// --- guess adapters: reuse the single heuristic implementation in PrinterHardware ---
std::string guess_part_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_part_cooling_fan();
}
std::string guess_hotend_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_hotend_fan();
}
std::string guess_chamber_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_chamber_fan();
}
std::string guess_exhaust_fan(const std::vector<std::string>& fans) {
    return PrinterHardware({}, {}, fans, {}).guess_exhaust_fan();
}
std::string guess_bed_heater(const std::vector<std::string>& heaters) {
    return PrinterHardware(heaters, {}, {}, {}).guess_bed_heater();
}
std::string guess_hotend_heater(const std::vector<std::string>& heaters) {
    return PrinterHardware(heaters, {}, {}, {}).guess_hotend_heater();
}

const std::vector<HardwareRoleDescriptor>& registry() {
    static const std::vector<HardwareRoleDescriptor> kReg = {
        {HardwareRoleId::HotendHeater, helix::wizard::HOTEND_HEATER, "extruder",
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true,
         &is_hotend_heater_candidate, &guess_hotend_heater},
        {HardwareRoleId::BedHeater, helix::wizard::BED_HEATER, "heater_bed",
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true,
         &is_bed_heater_candidate, &guess_bed_heater},
        {HardwareRoleId::PartFan, helix::wizard::PART_FAN, "fan", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_part_fan_candidate, &guess_part_fan},
        {HardwareRoleId::HotendFan, helix::wizard::HOTEND_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_hotend_fan_candidate, &guess_hotend_fan},
        {HardwareRoleId::ChamberFan, helix::wizard::CHAMBER_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_chamber_fan_candidate, &guess_chamber_fan},
        {HardwareRoleId::ExhaustFan, helix::wizard::EXHAUST_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, &is_exhaust_fan_candidate, &guess_exhaust_fan},
    };
    return kReg;
}

} // namespace

const std::vector<HardwareRoleDescriptor>& hardware_role_registry() {
    return registry();
}

const HardwareRoleDescriptor* role_descriptor(HardwareRoleId id) {
    for (const auto& d : registry()) {
        if (d.id == id)
            return &d;
    }
    return nullptr;
}

RoleResolution resolve_role(const HardwareRoleDescriptor& desc, const std::string& saved_value,
                            const std::vector<std::string>& discovered) {
    auto is_cand = [&](const std::string& o) {
        return desc.is_candidate == nullptr || desc.is_candidate(o);
    };

    // Tier 0: keep a still-valid saved role. User's explicit live choice is always honored —
    // candidacy is NOT checked here so a deliberately assigned wrong-category fan is kept.
    if (!saved_value.empty() && contains(discovered, saved_value)) {
        return {RoleResolutionStatus::Resolved, saved_value};
    }

    // Tier 1a: confident canonical default. Must pass candidacy so a wrong-category canonical
    // (edge case) is not silently accepted.
    if (desc.canonical_default && *desc.canonical_default &&
        contains(discovered, desc.canonical_default) && is_cand(desc.canonical_default)) {
        return {RoleResolutionStatus::AutoHealed, desc.canonical_default};
    }

    // Tier 1b: heuristic guess. Must pass candidacy so the fallback (e.g. fans_[0]) is not
    // accepted when it is a controller_fan / heater_fan.
    if (desc.guess) {
        std::string g = desc.guess(discovered);
        if (!g.empty() && contains(discovered, g) && is_cand(g)) {
            return {RoleResolutionStatus::AutoHealed, g};
        }
    }

    // Tier 2: cannot resolve confidently.
    return {RoleResolutionStatus::Unresolved, {}};
}

std::string resolve_role_from_config(HardwareRoleId id, Config* config,
                                     const std::vector<std::string>& discovered,
                                     bool persist_autoheal) {
    const HardwareRoleDescriptor* desc = role_descriptor(id);
    if (!desc || !config) {
        return {};
    }
    const std::string key = config->df() + desc->config_key;
    const std::string default_val =
        desc->canonical_default ? std::string(desc->canonical_default) : std::string();
    std::string saved = config->get<std::string>(key, default_val);

    // Empty saved means the role is intentionally unconfigured — do not invent one.
    if (saved.empty()) {
        return {};
    }

    RoleResolution res = resolve_role(*desc, saved, discovered);
    if (res.status == RoleResolutionStatus::AutoHealed) {
        spdlog::info("[HardwareRole] Auto-healed '{}' from '{}' to '{}' after hardware change",
                     desc->config_key, saved, res.object);
        if (persist_autoheal) {
            config->set<std::string>(key, res.object);
            if (!config->save()) {
                spdlog::warn("[HardwareRole] Failed to persist auto-heal for '{}'",
                             desc->config_key);
            }
        }
    } else if (res.status == RoleResolutionStatus::Unresolved) {
        spdlog::warn("[HardwareRole] Role '{}' (saved '{}') has no live match — unresolved",
                     desc->config_key, saved);
    }
    return res.object;
}

} // namespace helix

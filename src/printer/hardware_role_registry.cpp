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
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true, nullptr,
         &guess_hotend_heater},
        {HardwareRoleId::BedHeater, helix::wizard::BED_HEATER, "heater_bed",
         HardwareCategory::Heater, helix::wizard::StepId::HeaterSelect, true, nullptr,
         &guess_bed_heater},
        {HardwareRoleId::PartFan, helix::wizard::PART_FAN, "fan", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, nullptr, &guess_part_fan},
        {HardwareRoleId::HotendFan, helix::wizard::HOTEND_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, nullptr, &guess_hotend_fan},
        {HardwareRoleId::ChamberFan, helix::wizard::CHAMBER_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, nullptr, &guess_chamber_fan},
        {HardwareRoleId::ExhaustFan, helix::wizard::EXHAUST_FAN, "", HardwareCategory::Fan,
         helix::wizard::StepId::FanSelect, true, nullptr, &guess_exhaust_fan},
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

    // Tier 0: keep a still-valid saved role.
    if (!saved_value.empty() && contains(discovered, saved_value) && is_cand(saved_value)) {
        return {RoleResolutionStatus::Resolved, saved_value};
    }

    // Tier 1a: confident canonical default.
    if (desc.canonical_default && *desc.canonical_default &&
        contains(discovered, desc.canonical_default) && is_cand(desc.canonical_default)) {
        return {RoleResolutionStatus::AutoHealed, desc.canonical_default};
    }

    // Tier 1b: heuristic guess.
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
            config->save();
        }
    } else if (res.status == RoleResolutionStatus::Unresolved) {
        spdlog::warn("[HardwareRole] Role '{}' (saved '{}') has no live match — unresolved",
                     desc->config_key, saved);
    }
    return res.object;
}

} // namespace helix

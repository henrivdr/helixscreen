// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "hardware_role_registry.h"
#include "wizard_config_paths.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

static const HardwareRoleDescriptor& part_fan_desc() {
    return *role_descriptor(HardwareRoleId::PartFan);
}

TEST_CASE("resolve_role: Tier 0 keeps a live saved value", "[hwrole][resolve]") {
    auto r = resolve_role(part_fan_desc(), "fan_generic Aux_Cooling_Fan",
                          {"fan", "fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::Resolved);
    REQUIRE(r.object == "fan_generic Aux_Cooling_Fan");
}

TEST_CASE("resolve_role: bundle scenario auto-heals stale output_pin to canonical fan",
          "[hwrole][resolve]") {
    // ULAV93T2: fans/part=output_pin fan0 (silenced/absent), real [fan] present.
    auto r = resolve_role(part_fan_desc(), "output_pin fan0",
                          {"fan", "heater_fan hotend_fan", "fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::AutoHealed);
    REQUIRE(r.object == "fan");
}

TEST_CASE("resolve_role: auto-heals via guess when canonical absent", "[hwrole][resolve]") {
    // No bare "fan"; guess_part_cooling_fan falls back to first fan.
    auto r = resolve_role(part_fan_desc(), "output_pin fan0", {"fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::AutoHealed);
    REQUIRE(r.object == "fan_generic Aux_Cooling_Fan");
}

TEST_CASE("resolve_role: bed heater unresolved when no bed-ish heater exists",
          "[hwrole][resolve]") {
    const auto& bed = *role_descriptor(HardwareRoleId::BedHeater);
    auto r = resolve_role(bed, "heater_bed", {"extruder"});
    REQUIRE(r.status == RoleResolutionStatus::Unresolved);
    REQUIRE(r.object.empty());
}

TEST_CASE("resolve_role: bed heater keeps canonical when present", "[hwrole][resolve]") {
    const auto& bed = *role_descriptor(HardwareRoleId::BedHeater);
    auto r = resolve_role(bed, "heater_bed", {"extruder", "heater_bed"});
    REQUIRE(r.status == RoleResolutionStatus::Resolved);
    REQUIRE(r.object == "heater_bed");
}

TEST_CASE("resolve_role: Tier 1b never returns an object outside discovered", "[hwrole][resolve]") {
    const auto& part = *role_descriptor(HardwareRoleId::PartFan);
    auto r = resolve_role(part, "output_pin fan0", {}); // nothing live
    REQUIRE(r.status == RoleResolutionStatus::Unresolved);
    REQUIRE(r.object.empty());
}

TEST_CASE("registry integrity: every descriptor has a usable config key", "[hwrole][drift]") {
    const auto& reg = hardware_role_registry();
    REQUIRE(!reg.empty());
    for (const auto& d : reg) {
        REQUIRE(d.config_key != nullptr);
        REQUIRE(std::string(d.config_key).find("/") != std::string::npos);
        REQUIRE(role_descriptor(d.id) == &d);
    }
}

TEST_CASE("resolve_role_from_config: unconfigured optional role stays empty", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    cfg->set<std::string>(cfg->df() + helix::wizard::CHAMBER_FAN, std::string(""));
    std::string r = resolve_role_from_config(HardwareRoleId::ChamberFan, cfg,
                                             {"fan", "fan_generic chamber_fan"}, false);
    REQUIRE(r.empty()); // empty saved => do not invent a role
}

TEST_CASE("resolve_role_from_config: persists auto-healed part fan", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::PART_FAN, std::string("output_pin fan0"));
    std::string r = resolve_role_from_config(HardwareRoleId::PartFan, cfg,
                                             {"fan", "fan_generic Aux_Cooling_Fan"}, true);
    REQUIRE(r == "fan");
    REQUIRE(cfg->get<std::string>(cfg->df() + helix::wizard::PART_FAN, "") == "fan");
}

TEST_CASE("resolve_role_from_config: no persist leaves config untouched", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    cfg->set<std::string>(cfg->df() + helix::wizard::PART_FAN, std::string("output_pin fan0"));
    std::string r = resolve_role_from_config(HardwareRoleId::PartFan, cfg, {"fan"}, false);
    REQUIRE(r == "fan");
    REQUIRE(cfg->get<std::string>(cfg->df() + helix::wizard::PART_FAN, "") == "output_pin fan0");
}

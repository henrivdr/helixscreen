// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wizard_step.h" // helix::wizard::StepId

#include <string>
#include <vector>

namespace helix {

class Config;
class PrinterDiscovery;

enum class HardwareRoleId {
    HotendHeater,
    BedHeater,
    PartFan,
    HotendFan,
    ChamberFan,
    ExhaustFan,
};

enum class HardwareCategory { Fan, Heater, Led, FilamentSensor };

enum class RoleResolutionStatus {
    Resolved,   // saved value is live and valid -- keep it
    AutoHealed, // saved value invalid; a confident replacement was chosen
    Unresolved, // no confident replacement -- needs guided reconfig
};

struct RoleResolution {
    RoleResolutionStatus status;
    std::string object; // resolved object name; empty when Unresolved
};

/// One row of authoritative knowledge about a configurable hardware role.
/// is_candidate / guess may be nullptr (then: membership in `discovered` is the
/// only candidacy test, and no heuristic fallback is attempted, respectively).
struct HardwareRoleDescriptor {
    HardwareRoleId id;
    const char* config_key;        // relative to Config::df(), e.g. "fans/part"
    const char* canonical_default; // e.g. "fan", "heater_bed", "extruder", ""
    HardwareCategory category;
    helix::wizard::StepId wizard_step;
    bool guided;
    bool (*is_candidate)(const std::string& obj);
    std::string (*guess)(const std::vector<std::string>& objs);
};

/// The single source of truth. Stable order; safe to iterate.
const std::vector<HardwareRoleDescriptor>& hardware_role_registry();

/// Look up a descriptor by id. Returns nullptr if absent.
const HardwareRoleDescriptor* role_descriptor(HardwareRoleId id);

/// Pure tiered resolution -- no config, no persistence, fully unit-testable.
RoleResolution resolve_role(const HardwareRoleDescriptor& desc, const std::string& saved_value,
                            const std::vector<std::string>& discovered);

/// Read the saved role from config (default = canonical_default), resolve it
/// against `discovered`, optionally persist an AutoHealed value back to config.
/// Returns the resolved object name (empty if the role is unconfigured or
/// Unresolved). An empty saved value is treated as "unconfigured": returns ""
/// without inventing a role via heuristics.
std::string resolve_role_from_config(HardwareRoleId id, Config* config,
                                     const std::vector<std::string>& discovered,
                                     bool persist_autoheal);

/// Collect the deduped, registry-ordered set of wizard steps whose guided
/// hardware roles cannot be confidently resolved against discovered hardware.
///
/// For each guided registry descriptor: the discovered list is chosen by category
/// (Fan -> hw.fans(), Heater -> hw.heaters(); other categories are skipped). The
/// saved role is read from config (default = canonical_default). An empty saved
/// value is treated as unconfigured and skipped. When resolve_role() returns
/// Unresolved, the descriptor's wizard_step is added (once). Returns empty when
/// config is null or every guided role resolves.
std::vector<helix::wizard::StepId> unresolved_guided_steps(Config* config,
                                                           const PrinterDiscovery& hw);

} // namespace helix

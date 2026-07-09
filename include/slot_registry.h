// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace helix::printer {

/// Unified per-slot sensor state. Replaces AFC's LaneSensors and
/// Happy Hare's GateSensorState with a single struct usable by all backends.
struct SlotSensors {
    // AFC binary sensors
    bool prep = false;
    bool load = false;
    bool loaded_to_hub = false;

    // Happy Hare pre-gate sensor
    bool has_pre_gate_sensor = false;
    bool pre_gate_triggered = false;

    // AFC buffer/readiness
    std::string buffer_status;
    std::string filament_status;
    float dist_hub = 0.0f;
};

/// A single slot in the registry. Owns all per-slot state.
struct SlotEntry {
    int global_index = -1;
    int unit_index = -1;
    std::string backend_name; // "lane4" (AFC), "0" (HH) — for G-code

    SlotInfo info;
    SlotSensors sensors;
    int endless_spool_backup = -1;
};

/// Unit metadata in the registry.
struct RegistryUnit {
    std::string name;
    int first_slot = 0;
    int slot_count = 0;
};

/// Single source of truth for all slot-indexed state.
///
/// NOT thread-safe — callers must hold their own mutex.
/// No LVGL or Moonraker dependencies.
class SlotRegistry {
  public:
    // === Initialization ===
    void initialize(const std::string& unit_name, const std::vector<std::string>& slot_names);
    void
    initialize_units(const std::vector<std::pair<std::string, std::vector<std::string>>>& units);

    // === Reorganization (atomic) ===
    void
    reorganize(const std::vector<std::pair<std::string, std::vector<std::string>>>& unit_slot_map);

    // === Slot access ===
    int slot_count() const;
    bool is_valid_index(int global_index) const;
    const SlotEntry* get(int global_index) const;
    SlotEntry* get_mut(int global_index);
    const SlotEntry* find_by_name(const std::string& backend_name) const;
    SlotEntry* find_by_name_mut(const std::string& backend_name);
    int index_of(const std::string& backend_name) const;
    std::string name_of(int global_index) const;

    // === Unit access ===
    int unit_count() const;
    const RegistryUnit& unit(int unit_index) const;
    std::pair<int, int> unit_slot_range(int unit_index) const;
    int unit_for_slot(int global_index) const;

    // === Tool mapping ===
    int tool_for_slot(int global_index) const;
    int slot_for_tool(int tool_number) const;
    void set_tool_mapping(int global_index, int tool_number);
    void set_tool_map(const std::vector<int>& tool_to_slot);

    // === Endless spool ===
    int backup_for_slot(int global_index) const;
    void set_backup(int global_index, int backup_slot);

    // === Snapshot ===
    AmsSystemInfo build_system_info() const;

    // === Lifecycle ===
    bool is_initialized() const;
    void clear();

  private:
    std::vector<SlotEntry> slots_;
    std::unordered_map<std::string, int> name_to_index_;
    std::vector<int> tool_to_slot_;
    std::vector<RegistryUnit> units_;
    bool initialized_ = false;

    void rebuild_reverse_maps();
};

} // namespace helix::printer

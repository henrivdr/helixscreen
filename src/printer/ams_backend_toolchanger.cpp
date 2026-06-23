// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_toolchanger.h"

#include "ams_error.h"
#include "moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendToolChanger::AmsBackendToolChanger(MoonrakerAPI* api, MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info with tool changer defaults
    system_info_.type = AmsType::TOOL_CHANGER;
    system_info_.type_name = "Tool Changer";

    // Tool changer capabilities
    system_info_.supports_endless_spool = false; // Not applicable
    system_info_.supports_tool_mapping = true;   // Via klipper-toolchanger ASSIGN_TOOL
    system_info_.supports_bypass = false;        // No bypass on tool changers
    system_info_.has_hardware_bypass_sensor = false;

    spdlog::debug("[AMS ToolChanger] Backend created");
}

void AmsBackendToolChanger::set_discovered_tools(std::vector<std::string> tool_names) {
    std::lock_guard<std::mutex> lock(mutex_);

    tool_names_ = std::move(tool_names);

    // Initialize tool structures now that we have tool names
    if (!tool_names_.empty()) {
        initialize_tools();
    }

    spdlog::info("[AMS ToolChanger] Set {} discovered tools", tool_names_.size());
}

// Destructor not needed -- base class handles subscription cleanup

// ============================================================================
// Lifecycle Management
// ============================================================================

AmsError AmsBackendToolChanger::additional_start_checks() {
    if (tool_names_.empty()) {
        spdlog::error("[AMS ToolChanger] Cannot start: No tools discovered. "
                      "Call set_discovered_tools() before start()");
        return AmsErrorHelper::not_connected("No tools discovered");
    }
    return AmsErrorHelper::success();
}

// stop(), release_subscriptions(), is_running() provided by AmsSubscriptionBackend

// ============================================================================
// Event System
// ============================================================================

// set_event_callback() and emit_event() provided by AmsSubscriptionBackend

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendToolChanger::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

AmsType AmsBackendToolChanger::get_type() const {
    return AmsType::TOOL_CHANGER;
}

SlotInfo AmsBackendToolChanger::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }

    // Return empty slot info for invalid index
    SlotInfo empty;
    empty.slot_index = -1;
    return empty;
}

// get_current_action(), get_current_tool(), get_current_slot(), is_filament_loaded()
// provided by AmsSubscriptionBackend

// ============================================================================
// Path Visualization
// ============================================================================

PathTopology AmsBackendToolChanger::get_topology() const {
    return PathTopology::PARALLEL; // Each tool has its own independent path
}

PathSegment AmsBackendToolChanger::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // For tool changers, filament segment depends on whether a tool is mounted
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE; // Tool is mounted and active
    }
    return PathSegment::SPOOL; // No tool mounted (all tools in docks)
}

PathSegment AmsBackendToolChanger::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (slot_index < 0 || slot_index >= static_cast<int>(tool_mounted_.size())) {
        return PathSegment::NONE;
    }

    // For tool changers, each slot is a complete tool with filament loaded
    // through the nozzle — both mounted and docked tools have filament at nozzle
    return PathSegment::NOZZLE;
}

PathSegment AmsBackendToolChanger::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (system_info_.action == AmsAction::ERROR) {
        // For tool changers, errors typically occur at the dock or carriage
        return PathSegment::HUB; // Use HUB to represent the docking area
    }
    return PathSegment::NONE;
}

// ============================================================================
// Moonraker Status Update Handling
// ============================================================================

void AmsBackendToolChanger::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    bool state_changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check for toolchanger object updates
        if (params.contains("toolchanger")) {
            const auto& tc_data = params["toolchanger"];
            if (tc_data.is_object()) {
                spdlog::trace("[AMS ToolChanger] Received toolchanger status update");
                parse_toolchanger_state(tc_data);
                state_changed = true;
            }
        }

        // Check for individual tool updates (e.g., "tool T0", "tool T1")
        for (const auto& tool_name : tool_names_) {
            std::string key = "tool " + tool_name;
            if (params.contains(key)) {
                const auto& tool_data = params[key];
                if (tool_data.is_object()) {
                    spdlog::trace("[AMS ToolChanger] Received {} status update", key);
                    parse_tool_state(tool_name, tool_data);
                    state_changed = true;
                }
            }
        }
    }

    // Emit event OUTSIDE the lock to avoid deadlock if the callback
    // queries backend state (e.g., calls get_system_info() which acquires mutex_)
    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendToolChanger::parse_toolchanger_state(const nlohmann::json& tc_data) {
    // Parse status: toolchanger.status
    // Values: "ready", "changing", "error", "uninitialized"
    if (tc_data.contains("status") && tc_data["status"].is_string()) {
        std::string status_str = tc_data["status"].get<std::string>();
        system_info_.action = status_to_action(status_str);
        system_info_.operation_detail = status_str;
        spdlog::trace("[AMS ToolChanger] Status: {} -> {}", status_str,
                      ams_action_to_string(system_info_.action));
    }

    // Parse current tool: toolchanger.tool_number
    // -1 = no tool selected
    if (tc_data.contains("tool_number") && tc_data["tool_number"].is_number_integer()) {
        int tool_num = tc_data["tool_number"].get<int>();
        system_info_.current_tool = tool_num;
        system_info_.current_slot = tool_num; // For tool changers, slot == tool
        system_info_.filament_loaded = (tool_num >= 0);
        spdlog::trace("[AMS ToolChanger] Current tool: {}", tool_num);
    }

    // Parse tool list: toolchanger.tool_numbers and toolchanger.tool_names
    // This can be used to dynamically update the tool list
    if (tc_data.contains("tool_numbers") && tc_data["tool_numbers"].is_array()) {
        spdlog::trace("[AMS ToolChanger] Tool numbers: {}", tc_data["tool_numbers"].dump());
    }
}

void AmsBackendToolChanger::parse_tool_state(const std::string& tool_name,
                                             const nlohmann::json& tool_data) {
    int slot_idx = find_slot_for_tool(tool_name);
    if (slot_idx < 0) {
        spdlog::warn("[AMS ToolChanger] Unknown tool: {}", tool_name);
        return;
    }

    // Parse mounted state: tool.mounted
    if (tool_data.contains("mounted") && tool_data["mounted"].is_boolean()) {
        bool mounted = tool_data["mounted"].get<bool>();
        if (slot_idx < static_cast<int>(tool_mounted_.size())) {
            tool_mounted_[slot_idx] = mounted;
        }

        // Update slot status based on mounted state
        if (!system_info_.units.empty() &&
            slot_idx < static_cast<int>(system_info_.units[0].slots.size())) {
            system_info_.units[0].slots[slot_idx].status =
                mounted ? SlotStatus::LOADED : SlotStatus::AVAILABLE;
        }
        spdlog::trace("[AMS ToolChanger] Tool {} mounted: {}", tool_name, mounted);
    }

    // Parse active state: tool.active
    if (tool_data.contains("active") && tool_data["active"].is_boolean()) {
        bool active = tool_data["active"].get<bool>();
        spdlog::trace("[AMS ToolChanger] Tool {} active: {}", tool_name, active);
    }

    // Parse offsets (stored but not currently used in SlotInfo)
    if (tool_data.contains("gcode_x_offset") || tool_data.contains("gcode_y_offset") ||
        tool_data.contains("gcode_z_offset")) {
        spdlog::trace("[AMS ToolChanger] Tool {} has offset data", tool_name);
    }
}

AmsAction AmsBackendToolChanger::status_to_action(const std::string& status) {
    if (status == "ready") {
        return AmsAction::IDLE;
    }
    if (status == "changing") {
        return AmsAction::SELECTING;
    }
    if (status == "error") {
        return AmsAction::ERROR;
    }
    if (status == "uninitialized") {
        return AmsAction::RESETTING;
    }
    return AmsAction::IDLE;
}

void AmsBackendToolChanger::initialize_tools() {
    int tool_count = static_cast<int>(tool_names_.size());

    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "Tool Changer";
    unit.slot_count = tool_count;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.has_encoder = false;
    unit.has_toolhead_sensor = false;
    unit.has_slot_sensors = false;

    // Initialize slots for each tool
    tool_mounted_.clear();
    tool_mounted_.resize(tool_count, false);

    for (int i = 0; i < tool_count; ++i) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::AVAILABLE; // Tools start as available (docked)
        slot.mapped_tool = i;                // Tool i maps to slot i
        slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        slot.spool_name = tool_names_[i]; // Use tool name as placeholder

        unit.slots.push_back(slot);
    }

    system_info_.units.clear();
    system_info_.units.push_back(unit);
    system_info_.total_slots = tool_count;

    // Initialize tool-to-slot mapping (1:1 for tool changers)
    system_info_.tool_to_slot_map.clear();
    system_info_.tool_to_slot_map.reserve(tool_count);
    for (int i = 0; i < tool_count; ++i) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    tools_initialized_ = true;
    spdlog::info("[AMS ToolChanger] Initialized {} tools", tool_count);
}

int AmsBackendToolChanger::find_slot_for_tool(const std::string& tool_name) const {
    auto it = std::find(tool_names_.begin(), tool_names_.end(), tool_name);
    if (it != tool_names_.end()) {
        return static_cast<int>(std::distance(tool_names_.begin(), it));
    }
    return -1;
}

// ============================================================================
// Operations
// ============================================================================

// NOTE: Must be called while holding mutex_ (accesses system_info_ without lock)
// check_preconditions() provided by AmsSubscriptionBackend

// NOTE: Must be called while holding mutex_ (accesses system_info_ without lock)
AmsError AmsBackendToolChanger::validate_slot_index(int slot_index) const {
    // Special case: no tools discovered
    if (system_info_.total_slots == 0) {
        return AmsErrorHelper::not_connected("No tools discovered");
    }
    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }
    return AmsErrorHelper::success();
}

// execute_gcode() provided by AmsSubscriptionBackend

AmsError AmsBackendToolChanger::load_filament(int slot_index) {
    // For tool changers, "load filament" means "mount tool"
    return change_tool(slot_index);
}

AmsError AmsBackendToolChanger::unload_filament(int slot_index) {
    // For tool changers, "unload" means unmount a specific tool (or current if -1)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        if (slot_index >= 0) {
            AmsError slot_valid = validate_slot_index(slot_index);
            if (!slot_valid) {
                return slot_valid;
            }
        } else if (system_info_.current_tool < 0) {
            return AmsErrorHelper::not_loaded();
        }
    }

    if (slot_index >= 0) {
        std::string cmd = "UNSELECT_TOOL T=" + std::to_string(slot_index);
        spdlog::info("[AMS ToolChanger] Unmounting tool {}: {}", slot_index, cmd);
        return execute_gcode(cmd);
    }

    spdlog::info("[AMS ToolChanger] Unmounting current tool");
    return execute_gcode("UNSELECT_TOOL");
}

AmsError AmsBackendToolChanger::select_slot(int slot_index) {
    // For tool changers, selecting a slot means mounting that tool
    return change_tool(slot_index);
}

AmsError AmsBackendToolChanger::change_tool(int tool_number) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError precondition = check_preconditions();
        if (!precondition) {
            return precondition;
        }

        AmsError slot_valid = validate_slot_index(tool_number);
        if (!slot_valid) {
            return slot_valid;
        }
    }

    // Set action immediately to prevent race window where a second click
    // could arrive before Klipper's status update changes the action
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::SELECTING;
    }
    emit_event(EVENT_STATE_CHANGED);

    // Use SELECT_TOOL T={n} to select by tool number via the toolchanger's
    // internal lookup, bypassing any ASSIGN_TOOL T-command remapping.
    // This ensures we mount the physical tool the user tapped, not whatever
    // the slicer's T{n} command was remapped to.
    std::string cmd = "SELECT_TOOL T=" + std::to_string(tool_number);
    spdlog::info("[AMS ToolChanger] Mounting tool {}: {}", tool_number, cmd);
    return execute_gcode(cmd);
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendToolChanger::recover() {
    spdlog::info("[AMS ToolChanger] Attempting recovery");
    // klipper-toolchanger doesn't have a dedicated recovery command
    // Try to reinitialize the toolchanger
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

AmsError AmsBackendToolChanger::reset() {
    spdlog::info("[AMS ToolChanger] Resetting toolchanger");
    return execute_gcode("INITIALIZE_TOOLCHANGER");
}

AmsError AmsBackendToolChanger::cancel() {
    spdlog::info("[AMS ToolChanger] Cancel requested (not implemented for tool changers)");
    // Tool changes typically can't be cancelled mid-operation
    return AmsErrorHelper::not_supported("Cancel");
}

// ============================================================================
// Configuration Operations
// ============================================================================

AmsError AmsBackendToolChanger::set_slot_info(int slot_index, const SlotInfo& info,
                                              bool /*persist*/) {
    int old_mapped_tool = -1;
    std::string physical_tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        AmsError slot_valid = validate_slot_index(slot_index);
        if (!slot_valid) {
            return slot_valid;
        }

        // Update local state (for UI display)
        if (!system_info_.units.empty() &&
            slot_index < static_cast<int>(system_info_.units[0].slots.size())) {
            auto& slot = system_info_.units[0].slots[slot_index];
            old_mapped_tool = slot.mapped_tool;
            slot.color_rgb = info.color_rgb;
            slot.color_name = info.color_name;
            slot.material = info.material;
            slot.brand = info.brand;
            slot.spoolman_id = info.spoolman_id;
            slot.spool_name = info.spool_name;
            slot.remaining_weight_g = info.remaining_weight_g;
            slot.total_weight_g = info.total_weight_g;

            // Tool mapping change: persist via ASSIGN_TOOL outside the lock.
            // slot.mapped_tool stores "which G-code tool number activates this physical
            // tool"; the gcode says "T<info.mapped_tool> now activates tool_names_[slot]".
            if (info.mapped_tool != old_mapped_tool && info.mapped_tool >= 0 &&
                info.mapped_tool < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                slot_index < static_cast<int>(tool_names_.size())) {
                slot.mapped_tool = info.mapped_tool;
                system_info_.tool_to_slot_map[info.mapped_tool] = slot_index;
                physical_tool_name = tool_names_[slot_index];
            }
        }
    }

    if (!physical_tool_name.empty()) {
        spdlog::info("[AMS ToolChanger] Remap via slot edit: T{} -> physical {} (slot {})",
                     info.mapped_tool, physical_tool_name, slot_index);
        return execute_gcode(
            fmt::format("ASSIGN_TOOL TOOL={} N={}", physical_tool_name, info.mapped_tool));
    }

    return AmsErrorHelper::success();
}

AmsError AmsBackendToolChanger::set_tool_mapping(int tool_number, int slot_index) {
    // Remap G-code tool number to a different physical tool via klipper-toolchanger's
    // ASSIGN_TOOL command. This makes Klipper's T<tool_number> command activate the
    // physical tool at slot_index instead of tool_number.
    //
    // Example: set_tool_mapping(0, 2) sends "ASSIGN_TOOL TOOL=T2 N=0"
    //   → G-code "T0" now activates physical tool T2
    std::string physical_tool_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        int tool_count = static_cast<int>(tool_names_.size());
        if (tool_number < 0 || tool_number >= tool_count) {
            return AmsError(AmsResult::INVALID_TOOL,
                            "Tool " + std::to_string(tool_number) + " out of range",
                            "Invalid tool number", "");
        }
        if (slot_index < 0 || slot_index >= tool_count) {
            return AmsErrorHelper::invalid_slot(slot_index, tool_count - 1);
        }

        // The physical tool to assign (slot_index maps to tool_names_[slot_index])
        physical_tool_name = tool_names_[slot_index];

        // Update internal mapping
        if (tool_number < static_cast<int>(system_info_.tool_to_slot_map.size())) {
            system_info_.tool_to_slot_map[tool_number] = slot_index;
        }
    }

    // Send ASSIGN_TOOL: assign physical tool to respond to T<tool_number> commands
    std::ostringstream cmd;
    cmd << "ASSIGN_TOOL TOOL=" << physical_tool_name << " N=" << tool_number;

    spdlog::info("[AMS ToolChanger] Remapping T{} -> physical {} (slot {})", tool_number,
                 physical_tool_name, slot_index);
    return execute_gcode(cmd.str());
}

helix::printer::ToolMappingCapabilities
AmsBackendToolChanger::get_tool_mapping_capabilities() const {
    // klipper-toolchanger supports ASSIGN_TOOL for tool remapping
    return {true, true, "Tool reassignment via ASSIGN_TOOL"}; // i18n: do not translate
}

std::vector<int> AmsBackendToolChanger::get_tool_mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.tool_to_slot_map;
}

AmsError AmsBackendToolChanger::reset_tool_mappings() {
    // Restore identity mapping: each tool number maps to its own physical tool.
    // Sends ASSIGN_TOOL for each tool that was remapped away from identity.
    std::vector<std::pair<int, std::string>> remaps_needed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int tool_count = static_cast<int>(tool_names_.size());
        for (int i = 0; i < tool_count; ++i) {
            if (i < static_cast<int>(system_info_.tool_to_slot_map.size()) &&
                system_info_.tool_to_slot_map[i] != i) {
                // Tool i is currently mapped to a different slot — restore to identity
                remaps_needed.emplace_back(i, tool_names_[i]);
                system_info_.tool_to_slot_map[i] = i;
            }
        }
    }

    if (remaps_needed.empty()) {
        spdlog::debug("[AMS ToolChanger] All tools already at identity mapping");
        return AmsErrorHelper::success();
    }

    AmsError last_error = AmsErrorHelper::success();
    for (const auto& [tool_num, tool_name] : remaps_needed) {
        std::ostringstream cmd;
        cmd << "ASSIGN_TOOL TOOL=" << tool_name << " N=" << tool_num;
        spdlog::info("[AMS ToolChanger] Resetting T{} -> physical {} (identity)", tool_num,
                     tool_name);
        auto err = execute_gcode(cmd.str());
        if (err.result != AmsResult::SUCCESS) {
            last_error = err;
        }
    }

    spdlog::info("[AMS ToolChanger] Reset {} tool mapping(s) to identity", remaps_needed.size());
    return last_error;
}

// ============================================================================
// Bypass Mode (Not Applicable)
// ============================================================================

AmsError AmsBackendToolChanger::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendToolChanger::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

bool AmsBackendToolChanger::is_bypass_active() const {
    return false; // Tool changers never have bypass
}

// ============================================================================
// Device Actions (stub - not applicable for tool changers)
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendToolChanger::get_device_sections() const {
    // Tool changers don't expose device-specific actions
    return {};
}

std::vector<helix::printer::DeviceAction> AmsBackendToolChanger::get_device_actions() const {
    // Tool changers don't expose device-specific actions
    return {};
}

AmsError AmsBackendToolChanger::execute_device_action(const std::string& action_id,
                                                      const std::any& value) {
    (void)action_id;
    (void)value;
    return AmsErrorHelper::not_supported("Device actions");
}

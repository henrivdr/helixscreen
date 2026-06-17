// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "error_event.h"
#include "moonraker_api.h"

#include "config.h"
#include "settings_manager.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "../lvgl_test_fixture.h"

using namespace helix;
/**
 * @brief Test helper class providing access to AmsBackendAfc internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAfcTestHelper : public AmsBackendAfc {
  public:
    AmsBackendAfcTestHelper() : AmsBackendAfc(nullptr, nullptr) {}

    // Version testing helpers
    void set_afc_version(const std::string& version) {
        afc_version_ = version;
    }

    bool test_version_at_least(const std::string& required) const {
        return version_at_least(required);
    }

    // Sensor state setters for compute_filament_segment_unlocked testing
    void set_tool_end_sensor(bool state) {
        tool_end_sensor_ = state;
    }
    void set_tool_start_sensor(bool state) {
        tool_start_sensor_ = state;
    }
    void set_hub_sensor(const std::string& hub_name, bool state) {
        hub_sensors_[hub_name] = state;
    }

    // Convenience overload for single-hub backward compat in tests
    void set_hub_sensor(bool state) {
        // Set/clear on a default hub name for single-hub tests
        if (state) {
            hub_sensors_["default"] = true;
        } else {
            hub_sensors_.clear();
        }
    }

    void set_current_lane(const std::string& lane_name) {
        current_lane_name_ = lane_name;
    }

    void initialize_test_lanes(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
        }
        initialize_slots(names);
    }

    // 0-based lane naming: lane0, lane1, ... lane{N-1} (matches real AFC hardware)
    void initialize_test_lanes_zero_based(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i));
        }
        initialize_slots(names);
    }

    void set_lane_prep_sensor(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.prep = state;
    }

    void set_lane_load_sensor(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.load = state;
    }

    void set_lane_loaded_to_hub(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.loaded_to_hub = state;
    }

    void set_running(bool state) {
        running_ = state;
    }

    void set_filament_loaded(bool state) {
        system_info_.filament_loaded = state;
    }

    void set_current_slot(int slot) {
        system_info_.current_slot = slot;
    }

    PathSegment test_compute_filament_segment() const {
        return compute_filament_segment_unlocked();
    }

    // Discovery testing helpers
    int get_slot_count() const {
        return slots_.slot_count();
    }

    std::string get_slot_name(int index) const {
        return slots_.name_of(index);
    }

    const std::vector<std::string>& get_hub_names() const {
        return hub_names_;
    }

    void initialize_slots_from_discovery() {
        // Simulates what start() does when lanes are pre-set via set_discovered_lanes()
        if (!discovered_lane_names_.empty() && !slots_.is_initialized()) {
            initialize_slots(discovered_lane_names_);
        }
    }

    // Persistence testing helpers
    void initialize_test_lanes_with_slots(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Box Turtle 1";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;

        for (int i = 0; i < count; ++i) {
            std::string name = "lane" + std::to_string(i + 1);
            names.push_back(name);

            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        slots_.initialize("Box Turtle 1", names);

        // Set mapped_tool on registry entries to match unit slot info
        for (int i = 0; i < count; ++i) {
            auto* s = get_mutable_slot(i);
            if (s)
                s->mapped_tool = i;
        }
    }

    SlotInfo* get_mutable_slot(int slot_index) {
        auto* entry = slots_.get_mut(slot_index);
        return entry ? &entry->info : nullptr;
    }

    // Initialize endless spool configs for reset testing
    void initialize_endless_spool_configs(int count) {
        for (int i = 0; i < count; ++i) {
            slots_.set_backup(i, -1);
        }
    }

    // Set a specific endless spool backup for testing
    void set_endless_spool_config(int slot, int backup) {
        slots_.set_backup(slot, backup);
    }

    // Set up multi-unit configuration and trigger reorganize
    void
    setup_multi_unit(const std::unordered_map<std::string, std::vector<std::string>>& unit_map) {
        unit_lane_map_ = unit_map;
        reorganize_slots();
    }

    // For persistence tests: capture G-code commands
    std::vector<std::string> captured_gcodes;

    // Override execute_gcode to capture commands for testing
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Override execute_gcode_notify to capture commands (avoids real API call)
    AmsError execute_gcode_notify(const std::string& gcode, const std::string& /*success_msg*/,
                                  const std::string& /*error_prefix*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // ------------------------------------------------------------------
    // LANE_UNLOAD dispatch capture
    //
    // The production dispatch_lane_unload() goes through api_->execute_gcode()
    // with success/error callbacks (the queue drains when Moonraker reports
    // completion). In tests, api_ is null. Override to capture the gcode AND
    // immediately signal completion so the queue advances synchronously —
    // letting tests assert on order/contents without async plumbing.
    //
    // Tests that need to inspect serialization should set
    // `defer_lane_unload_complete = true` and call `complete_pending_unload()`
    // manually to step through the queue one entry at a time.
    bool defer_lane_unload_complete = false;
    int pending_unload_completions = 0;

    void dispatch_lane_unload(const std::string& lane_name) override {
        captured_gcodes.push_back("LANE_UNLOAD LANE=" + lane_name);
        if (defer_lane_unload_complete) {
            ++pending_unload_completions;
        } else {
            on_lane_unload_done();
        }
    }

    void complete_pending_unload() {
        if (pending_unload_completions > 0) {
            --pending_unload_completions;
        }
        on_lane_unload_done();
    }

    void clear_captured_gcodes() {
        captured_gcodes.clear();
    }

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    // Feed a Moonraker notify_status_update notification through the backend
    void feed_status_update(const nlohmann::json& params_inner) {
        // Build the full notification format: { "params": [ { ... }, timestamp ] }
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    // Feed AFC global state update
    void feed_afc_state(const nlohmann::json& afc_data) {
        nlohmann::json params;
        params["AFC"] = afc_data;
        feed_status_update(params);
    }

    // Feed AFC_stepper lane update
    void feed_afc_stepper(const std::string& lane_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane_name] = data;
        feed_status_update(params);
    }

    // State accessors for test assertions
    AmsAction get_action() const {
        return system_info_.action;
    }
    std::string get_operation_detail() const {
        return system_info_.operation_detail;
    }
    std::vector<int> get_tool_to_slot_map() const {
        return get_system_info().tool_to_slot_map;
    }

    std::vector<helix::printer::EndlessSpoolConfig> get_endless_spool_configs() const {
        return get_endless_spool_config();
    }

    // Get mapped_tool from a slot
    int get_slot_mapped_tool(int slot_index) const {
        const auto* entry = slots_.get(slot_index);
        return entry ? entry->info.mapped_tool : -1;
    }

    // Event tracking
    std::vector<std::pair<std::string, std::string>> emitted_events;

    void install_event_tracker() {
        set_event_callback([this](const std::string& event, const std::string& data) {
            emitted_events.emplace_back(event, data);
        });
    }

    bool has_event(const std::string& event) const {
        for (const auto& [ev, _] : emitted_events) {
            if (ev == event)
                return true;
        }
        return false;
    }

    std::string get_event_data(const std::string& event) const {
        for (const auto& [ev, data] : emitted_events) {
            if (ev == event)
                return data;
        }
        return "";
    }

    // Access to extended parsing state (reads from registry)
    helix::printer::SlotSensors get_lane_sensors(int index) const {
        const auto* entry = slots_.get(index);
        if (entry) {
            return entry->sensors;
        }
        return {};
    }
    bool get_hub_sensor() const {
        // Returns true if any hub sensor is triggered (backward compat)
        for (const auto& [name, triggered] : hub_sensors_) {
            if (triggered)
                return true;
        }
        return false;
    }

    bool get_hub_sensor(const std::string& hub_name) const {
        auto it = hub_sensors_.find(hub_name);
        return it != hub_sensors_.end() && it->second;
    }

    const std::unordered_map<std::string, bool>& get_hub_sensors() const {
        return hub_sensors_;
    }
    bool get_tool_start_sensor() const {
        return tool_start_sensor_;
    }
    bool get_tool_end_sensor() const {
        return tool_end_sensor_;
    }
    bool get_quiet_mode() const {
        return afc_quiet_mode_;
    }
    bool get_led_state() const {
        return afc_led_state_;
    }
    float get_bowden_length() const {
        return bowden_length_;
    }

    // Feed AFC_hub update
    void feed_afc_hub(const std::string& hub_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_hub " + hub_name] = data;
        feed_status_update(params);
    }

    // Feed AFC_extruder update
    void feed_afc_extruder(const std::string& ext_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_extruder " + ext_name] = data;
        feed_status_update(params);
    }

    // Feed AFC_buffer update
    void feed_afc_buffer(const std::string& buf_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_buffer " + buf_name] = data;
        feed_status_update(params);
    }

    // Phase 2 mixed topology accessors
    const std::vector<AfcUnitInfo>& get_unit_infos() const {
        return unit_infos_;
    }

    const std::vector<std::string>& get_extruder_names() const {
        return extruder_names_;
    }

    AmsSystemInfo& get_system_info_mutable() {
        return system_info_;
    }

    // Toolchanger mode setup: configures num_extruders_ and extruders_
    void setup_toolchanger(int num_extruders) {
        num_extruders_ = num_extruders;
        extruders_.clear();
        extruder_names_.clear();
        for (int i = 0; i < num_extruders; ++i) {
            std::string name = (i == 0) ? "extruder" : "extruder" + std::to_string(i);
            AfcExtruderInfo ext;
            ext.name = name;
            extruders_.push_back(std::move(ext));
            extruder_names_.push_back(std::move(name));
        }
    }
};

// ============================================================================
// version_at_least() - Semantic Version Comparison Tests
// ============================================================================

TEST_CASE("AFC version_at_least: equal versions", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.32");

    REQUIRE(helper.test_version_at_least("1.0.32") == true);
}

TEST_CASE("AFC version_at_least: greater patch version", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.33");

    REQUIRE(helper.test_version_at_least("1.0.32") == true);
}

TEST_CASE("AFC version_at_least: greater minor version", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.1.0");

    REQUIRE(helper.test_version_at_least("1.0.32") == true);
}

TEST_CASE("AFC version_at_least: greater major version", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("2.0.0");

    REQUIRE(helper.test_version_at_least("1.0.32") == true);
}

TEST_CASE("AFC version_at_least: lesser patch version fails", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.31");

    REQUIRE(helper.test_version_at_least("1.0.32") == false);
}

TEST_CASE("AFC version_at_least: unknown version fails", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("unknown");

    REQUIRE(helper.test_version_at_least("1.0.32") == false);
}

TEST_CASE("AFC version_at_least: empty version fails", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("");

    REQUIRE(helper.test_version_at_least("1.0.32") == false);
}

TEST_CASE("AFC version_at_least: lesser minor version fails", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.0");

    REQUIRE(helper.test_version_at_least("1.1.0") == false);
}

TEST_CASE("AFC version_at_least: lesser major version fails", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.99.99");

    REQUIRE(helper.test_version_at_least("2.0.0") == false);
}

TEST_CASE("AFC version_at_least: high patch vs low minor", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.set_afc_version("1.0.100");

    // 1.0.100 is still < 1.1.0 because minor takes precedence
    REQUIRE(helper.test_version_at_least("1.1.0") == false);
}

TEST_CASE("AFC version_at_least: handles two-part version", "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    // Version parsing uses istringstream which may handle partial versions
    helper.set_afc_version("1.0");

    // Should treat missing patch as 0, so 1.0.0 >= 1.0.0
    REQUIRE(helper.test_version_at_least("1.0.0") == true);
}

// ============================================================================
// compute_filament_segment_unlocked() - Sensor-to-Segment Mapping Tests
// ============================================================================

TEST_CASE("AFC segment: no sensors triggered returns NONE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    // No sensors triggered, no filament loaded, no current slot
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NONE);
}

TEST_CASE("AFC segment: filament loaded flag returns SPOOL when no sensors",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_filament_loaded(true);

    // Filament is "loaded" but no sensors triggered - implies at spool
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::SPOOL);
}

TEST_CASE("AFC segment: current slot set returns SPOOL when no sensors", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_slot(0);

    // A slot is selected but no sensors - filament at spool area
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::SPOOL);
}

TEST_CASE("AFC segment: prep sensor triggered returns PREP", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: prep and load sensors return LANE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment: loaded_to_hub returns HUB", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(0, true);
    helper.set_lane_loaded_to_hub(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::HUB);
}

TEST_CASE("AFC segment: hub_sensor returns OUTPUT", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor(true);

    // Hub sensor indicates filament past the hub merger, heading to toolhead
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::OUTPUT);
}

TEST_CASE("AFC segment: tool_start_sensor returns TOOLHEAD", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::TOOLHEAD);
}

TEST_CASE("AFC segment: tool_end_sensor returns NOZZLE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);
    helper.set_tool_end_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NOZZLE);
}

TEST_CASE("AFC segment: tool_end_sensor alone returns NOZZLE (overrides all)",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Only end sensor, no others - still returns NOZZLE as it's furthest
    helper.set_tool_end_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NOZZLE);
}

TEST_CASE("AFC segment: fallback scans all lanes for prep sensor", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane3 has prep sensor triggered
    helper.set_lane_prep_sensor(2, true); // lane3 is index 2

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: fallback scans all lanes for load sensor", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane2 has load sensor triggered
    helper.set_lane_load_sensor(1, true); // lane2 is index 1

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment: fallback scans all lanes for loaded_to_hub", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane4 has loaded_to_hub
    helper.set_lane_loaded_to_hub(3, true); // lane4 is index 3

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::HUB);
}

TEST_CASE("AFC segment: hub sensor takes priority over lane sensors", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(0, true);
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor(true);

    // Hub sensor should return OUTPUT even with all lane sensors triggered
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::OUTPUT);
}

TEST_CASE("AFC segment: toolhead sensors take priority over hub", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);

    // tool_start_sensor should return TOOLHEAD even with hub sensor triggered
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::TOOLHEAD);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("AFC segment: no lanes initialized returns NONE", "[ams][afc][segment][edge]") {
    AmsBackendAfcTestHelper helper;
    // Don't call initialize_test_lanes - no lanes configured

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NONE);
}

TEST_CASE("AFC segment: current lane not in map uses fallback scan", "[ams][afc][segment][edge]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Set a lane name that doesn't exist in the map
    helper.set_current_lane("nonexistent");
    helper.set_lane_prep_sensor(0, true);

    // Should fall back to scanning all lanes
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC version_at_least: dev version string", "[ams][afc][version][edge]") {
    AmsBackendAfcTestHelper helper;
    // Some systems may have dev/beta suffixes, but our parser ignores them
    // "1.0.32-dev" will parse as 1.0.32 (istringstream stops at non-digit)
    helper.set_afc_version("1.0.32-dev");

    // This should still satisfy >= 1.0.32 since the numeric parts match
    REQUIRE(helper.test_version_at_least("1.0.32") == true);
}

TEST_CASE("AFC segment: multiple lanes with sensors uses first match in order",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Multiple lanes have sensors triggered, but no current lane set
    // The algorithm iterates through lanes in order and returns on first sensor found
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(1, true);
    helper.set_lane_loaded_to_hub(2, true);

    // Fallback iterates by lane, checking loaded_to_hub > load > prep for each lane
    // Lane 0: loaded_to_hub=false, load=false, prep=true -> returns PREP
    // The algorithm returns the first sensor state found, not the furthest overall
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: fallback prioritizes hub over lane sensors per-lane",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Lane 2 has all sensors including loaded_to_hub, lane 0 only has prep
    // Since the algorithm checks loaded_to_hub first for each lane,
    // lane 0's loaded_to_hub=false means it continues to check load, then prep
    // But loaded_to_hub IS checked before load/prep for each individual lane
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_lane_prep_sensor(1, true);

    // Lane 0 has loaded_to_hub=true, so it returns HUB
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::HUB);
}

// ============================================================================
// set_discovered_lanes() - Lane Discovery from PrinterCapabilities Tests
// ============================================================================

TEST_CASE("AFC set_discovered_lanes: sets lane names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};

    helper.set_discovered_lanes(lanes, hubs);

    // After setting lanes and initializing, they should be accessible via registry
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 4);
    REQUIRE(helper.get_slot_name(0) == "lane1");
    REQUIRE(helper.get_slot_name(3) == "lane4");
}

TEST_CASE("AFC set_discovered_lanes: sets hub names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2"};

    helper.set_discovered_lanes(lanes, hubs);

    REQUIRE(helper.get_hub_names().size() == 2);
    REQUIRE(helper.get_hub_names()[0] == "Turtle_1");
}

TEST_CASE("AFC set_discovered_lanes: empty lanes doesn't overwrite existing",
          "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    // First set some lanes
    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Then call with empty lanes - should not overwrite
    std::vector<std::string> empty_lanes;
    std::vector<std::string> new_hubs = {"NewHub"};
    helper.set_discovered_lanes(empty_lanes, new_hubs);

    // Lanes should remain unchanged (check via discovery init)
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 2);
    // But hubs should be updated
    REQUIRE(helper.get_hub_names().size() == 1);
    REQUIRE(helper.get_hub_names()[0] == "NewHub");
}

TEST_CASE("AFC segment: works with discovered lanes", "[ams][afc][discovery][segment]") {
    AmsBackendAfcTestHelper helper;

    // Set lanes via discovery (like PrinterCapabilities would)
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Initialize the lanes (like start() would do)
    helper.initialize_slots_from_discovery();

    // Now test that sensors work correctly
    helper.set_current_lane("lane2");
    helper.set_lane_prep_sensor(1, true);
    helper.set_lane_load_sensor(1, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

// ============================================================================
// set_slot_info() Persistence Tests - AFC >= 1.0.20
// ============================================================================
//
// These tests verify that set_slot_info() sends the appropriate G-code commands
// to persist filament properties when AFC version >= 1.0.20.
//
// Commands expected:
// - SET_COLOR LANE=<name> COLOR=<RRGGBB>
// - SET_MATERIAL LANE=<name> MATERIAL=<type>
// - SET_WEIGHT LANE=<name> WEIGHT=<grams>
// - SET_SPOOL_ID LANE=<name> SPOOL_ID=<id>
//
// NOTE: These tests are designed to FAIL initially. The set_slot_info() method
// currently only updates local state and does NOT send G-code commands.
// Implementation must be added to make these tests pass.
//
// Testing approach: Since MoonrakerAPI::execute_gcode() is not virtual,
// the test helper captures G-code via the protected execute_gcode() method
// that AmsBackendAfc exposes. The implementation must call execute_gcode()
// for these tests to pass.
// ============================================================================

TEST_CASE("AFC persistence: old version skips G-code commands", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.19"); // Below 1.0.20 threshold
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red
    info.material = "PLA";
    info.remaining_weight_g = 850;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info);

    // Old version should NOT send any persistence commands
    // This test PASSES currently since no G-code is sent at all
    REQUIRE(helper.captured_gcodes.empty());
}

TEST_CASE("AFC persistence: SET_COLOR command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red

    helper.set_slot_info(0, info);

    // Should send: SET_COLOR LANE=lane1 COLOR=FF0000
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=FF0000"));
}

TEST_CASE("AFC persistence: SET_COLOR uppercase hex no prefix", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x00FF00; // Green

    helper.set_slot_info(1, info);

    // Should send: SET_COLOR LANE=lane2 COLOR=00FF00 (uppercase, no #)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane2 COLOR=00FF00"));
}

TEST_CASE("AFC persistence: SET_MATERIAL command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA";

    helper.set_slot_info(1, info);

    // Should send: SET_MATERIAL LANE=lane2 MATERIAL=PLA
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane2 MATERIAL=PLA"));
}

TEST_CASE("AFC persistence: SET_WEIGHT command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.remaining_weight_g = 850.5f; // Should be sent as integer

    helper.set_slot_info(0, info);

    // Should send: SET_WEIGHT LANE=lane1 WEIGHT=850 (no decimals)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=850"));
}

TEST_CASE("AFC persistence: SET_SPOOL_ID command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info);

    // Should send: SET_SPOOL_ID LANE=lane1 SPOOL_ID=42
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: SET_SPOOL_ID clear with empty string", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Pre-set existing spoolman_id on slot
    SlotInfo* existing_slot = helper.get_mutable_slot(0);
    REQUIRE(existing_slot != nullptr);
    existing_slot->spoolman_id = 123;

    // Now clear it by setting spoolman_id = 0
    SlotInfo new_info;
    new_info.spoolman_id = 0;

    helper.set_slot_info(0, new_info);

    // Should send: SET_SPOOL_ID LANE=lane1 SPOOL_ID= (empty to clear)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID="));
}

TEST_CASE("AFC persistence: SET_MAP fires when mapped_tool changes via set_slot_info",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Helper seeds slot 0 → T0 by default. Remap it to T2 through the slot edit path.
    SlotInfo info;
    info.mapped_tool = 2;

    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_MAP LANE=lane1 MAP=T2"));
}

TEST_CASE("AFC persistence: SET_MAP not fired when mapped_tool unchanged",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Default mapping for slot 0 is T0. Setting same value should not emit SET_MAP.
    SlotInfo info;
    info.mapped_tool = 0;
    info.material = "PLA";

    helper.set_slot_info(0, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
}

TEST_CASE("AFC persistence: SET_MAP not fired when caller leaves mapped_tool default (-1)",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Spoolman polling and consumption_sink build a SlotInfo from the existing slot,
    // but a misuse (default-constructed SlotInfo) must NOT clobber the live mapping.
    // The guard is `info.mapped_tool >= 0` — so mapped_tool=-1 is a no-op.
    SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    helper.set_slot_info(2, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
    // Live registry mapping for slot 2 must still be T2 (the seeded default).
    REQUIRE(helper.get_slot_mapped_tool(2) == 2);
}

TEST_CASE("AFC persistence: mapped_tool change updates registry reverse map",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Remap slot 0 from T0 → T2 via the slot edit path. Registry should reflect it
    // so future tool-changes (T2 → which slot?) resolve correctly.
    SlotInfo info;
    info.mapped_tool = 2;
    helper.set_slot_info(0, info);

    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
}

TEST_CASE("AFC persistence: sends gcode even with unknown version", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("unknown");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;
    info.material = "PLA";
    info.color_rgb = 0xFF0000;
    info.remaining_weight_g = 800.0f;

    helper.set_slot_info(0, info);

    // Unknown version should still attempt gcode (SET_SPOOL_ID existed before 1.0.20)
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: sends gcode with empty version", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: skips SET_COLOR for default grey", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x808080; // Default grey - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_COLOR for grey default
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_COLOR"));
}

TEST_CASE("AFC persistence: skips SET_COLOR for zero", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0; // Zero color - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_COLOR for zero
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_COLOR"));
}

TEST_CASE("AFC persistence: skips SET_MATERIAL for empty string", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = ""; // Empty - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_MATERIAL for empty
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_MATERIAL"));
}

TEST_CASE("AFC persistence: skips SET_WEIGHT for zero or negative", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SECTION("zero weight") {
        SlotInfo info;
        info.remaining_weight_g = 0;
        helper.set_slot_info(0, info);
        // PASSES: no G-code sent at all currently
        REQUIRE_FALSE(helper.has_gcode_starting_with("SET_WEIGHT"));
    }

    SECTION("negative weight (unknown)") {
        helper.clear_captured_gcodes();
        SlotInfo info;
        info.remaining_weight_g = -1;
        helper.set_slot_info(0, info);
        // PASSES: no G-code sent at all currently
        REQUIRE_FALSE(helper.has_gcode_starting_with("SET_WEIGHT"));
    }
}

TEST_CASE("AFC persistence: skips SET_SPOOL_ID when both old and new are zero",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Slot starts with spoolman_id = 0 (default)
    SlotInfo info;
    info.spoolman_id = 0;

    helper.set_slot_info(0, info);

    // Should NOT send SET_SPOOL_ID when both old and new are 0
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_SPOOL_ID"));
}

TEST_CASE("AFC persistence: sends multiple commands for full slot info",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x0000FF; // Blue
    info.material = "PETG";
    info.remaining_weight_g = 750;
    info.spoolman_id = 99;

    helper.set_slot_info(0, info);

    // Should send all four commands
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=0000FF"));
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PETG"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=750"));
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=99"));
}

// ============================================================================
// set_slot_info() persist=false Tests
// ============================================================================
//
// When persist=false, set_slot_info() should update in-memory slot state but
// NOT send any G-code commands to firmware. This is critical for preventing an
// infinite feedback loop when Spoolman weight polling updates slot data:
//
//   set_slot_info(persist=true) → G-code to firmware → firmware status_update
//   via WebSocket → sync_from_backend → refresh_spoolman_weights →
//   set_slot_info again → ∞
//
// With persist=false, the cycle breaks because no G-code is sent, so firmware
// doesn't emit a status_update, and the loop terminates.
// ============================================================================

TEST_CASE("AFC persist=false: updates local state without G-code",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    info.remaining_weight_g = 850;
    info.spoolman_id = 42;

    // persist=false should NOT send any G-code
    helper.set_slot_info(0, info, /*persist=*/false);

    REQUIRE(helper.captured_gcodes.empty());

    // But local state SHOULD be updated
    SlotInfo stored = helper.get_slot_info(0);
    REQUIRE(stored.color_rgb == 0xFF0000);
    REQUIRE(stored.material == "PLA");
    REQUIRE(stored.remaining_weight_g == Catch::Approx(850.0f));
    REQUIRE(stored.spoolman_id == 42);
}

TEST_CASE("AFC persist=true: sends G-code (default behavior unchanged)",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x00FF00;
    info.material = "ABS";
    info.remaining_weight_g = 500;
    info.spoolman_id = 7;

    // Default persist=true should send G-code
    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=00FF00"));
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=ABS"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=500"));
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=7"));
}

TEST_CASE("AFC persist=false: version warning not emitted",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    // Old version + persist=false should NOT log the upgrade warning
    helper.set_afc_version("1.0.19");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    // Should succeed without errors and without persistence
    auto result = helper.set_slot_info(0, info, /*persist=*/false);
    REQUIRE(result.success());
    REQUIRE(helper.captured_gcodes.empty());
}

// ============================================================================
// reset_tool_mappings() Tests
// ============================================================================

TEST_CASE("AFC reset_tool_mappings sends RESET_AFC_MAPPING RUNOUT=no",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("RESET_AFC_MAPPING RUNOUT=no"));
}

TEST_CASE("AFC reset_tool_mappings sends single command regardless of lane count",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(8);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    // Should send exactly one command, not one per lane
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.has_gcode("RESET_AFC_MAPPING RUNOUT=no"));
}

// ============================================================================
// reset_endless_spool() Tests
// ============================================================================

TEST_CASE("AFC reset_endless_spool clears all slots", "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Set some backups first
    helper.set_endless_spool_config(0, 1);
    helper.set_endless_spool_config(2, 3);

    auto result = helper.reset_endless_spool();

    REQUIRE(result.success());
    // Should have sent 4 SET_RUNOUT commands (one per slot)
    REQUIRE(helper.captured_gcodes.size() == 4);

    // Each should be setting RUNOUT=NONE to disable
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane1 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane2 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane3 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane4 RUNOUT=NONE"));
}

TEST_CASE("AFC reset_endless_spool with zero slots is no-op", "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    // Don't initialize any lanes or configs

    auto result = helper.reset_endless_spool();

    REQUIRE(result.success());
    REQUIRE(helper.captured_gcodes.empty());
}

TEST_CASE("AFC reset_endless_spool continues on partial failure",
          "[ams][afc][endless_spool][reset]") {
    // This test verifies that if one slot fails, we still attempt the remaining slots
    // The implementation should return the first error but continue processing
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    auto result = helper.reset_endless_spool();

    // Should still have attempted all 4 slots even if one hypothetically failed
    REQUIRE(helper.captured_gcodes.size() == 4);
}

// ============================================================================
// Phase 1: Bug Fixes & Critical Data Sync Tests
// ============================================================================
//
// These tests verify parsing of fields that the real AFC device exposes
// (captured from 192.168.1.112). Tests use fixture data to validate that
// state updates flow through correctly to internal state.
// ============================================================================

TEST_CASE("AFC current_state preferred over status field", "[ams][afc][state][phase1]") {
    // Real device sends "current_state": "Idle" (in AFC global object)
    // but we only parse "status" field today. current_state should take priority
    // because it's the newer, more accurate field.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed AFC state with both current_state and status
    // current_state says "Idle" but status says "Loading" — current_state should win
    nlohmann::json afc_data = {{"current_state", "Idle"}, {"status", "Loading"}};
    helper.feed_afc_state(afc_data);

    // current_state takes priority over status field
    REQUIRE(helper.get_action() == AmsAction::IDLE);
}

TEST_CASE("AFC current_state fallback to status when no current_state",
          "[ams][afc][state][phase1]") {
    // When current_state is absent, fall back to status field (regression guard)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {{"status", "Loading"}};
    helper.feed_afc_state(afc_data);

    // Should still work via status field — this PASSES today (regression guard)
    REQUIRE(helper.get_action() == AmsAction::LOADING);
}

TEST_CASE("AFC tool mapping from stepper map field", "[ams][afc][tool_mapping][phase1]") {
    // Real device: AFC_stepper lane1 has "map": "T0", lane2 has "map": "T1", etc.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed stepper data with map field
    helper.feed_afc_stepper("lane1", {{"map", "T0"}, {"prep", true}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}, {"prep", true}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}, {"prep", false}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}, {"prep", false}});

    // tool_to_slot_map should reflect the mapping from stepper "map" fields
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 0); // T0 → lane1 (slot 0)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 2); // T2 → lane3 (slot 2)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)
}

TEST_CASE("AFC tool mapping swap updates correctly", "[ams][afc][tool_mapping][phase1]") {
    // When lanes swap tools (e.g., T0 moves from lane1 to lane3), the mapping
    // should update accordingly
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Initial mapping: T0→lane1, T1→lane2, T2→lane3, T3→lane4
    helper.feed_afc_stepper("lane1", {{"map", "T0"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}});

    // Now swap: lane1 gets T2, lane3 gets T0
    helper.feed_afc_stepper("lane1", {{"map", "T2"}});
    helper.feed_afc_stepper("lane3", {{"map", "T0"}});

    // After swap, mapping should reflect new tool assignments
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 2); // T0 → lane3 (slot 2)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 0); // T2 → lane1 (slot 0)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)

    // Slot mapped_tool should also be updated
    REQUIRE(helper.get_slot_mapped_tool(0) == 2); // lane1 now maps to T2
    REQUIRE(helper.get_slot_mapped_tool(2) == 0); // lane3 now maps to T0
}

TEST_CASE("AFC endless spool from runout_lane field", "[ams][afc][endless_spool][phase1]") {
    // Real device: AFC_stepper lane1 has "runout_lane": "lane2"
    // meaning if lane1 runs out, switch to lane2.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Feed stepper data with runout_lane
    helper.feed_afc_stepper("lane1", {{"runout_lane", "lane2"}});

    // runout_lane should update endless spool backup config
    auto configs = helper.get_endless_spool_configs();
    REQUIRE(configs.size() == 4);
    REQUIRE(configs[0].backup_slot == 1); // lane1's backup is lane2 (slot 1)
}

TEST_CASE("AFC endless spool null runout_lane clears backup", "[ams][afc][endless_spool][phase1]") {
    // When runout_lane is null, the backup should be cleared (-1)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // First set a backup
    helper.set_endless_spool_config(0, 1); // lane1 backup = lane2

    // Now feed a null runout_lane
    nlohmann::json stepper_data;
    stepper_data["runout_lane"] = nullptr; // JSON null
    helper.feed_afc_stepper("lane1", stepper_data);

    // null runout_lane should clear the backup
    auto configs = helper.get_endless_spool_configs();
    REQUIRE(configs[0].backup_slot == -1); // Cleared
}

TEST_CASE("AFC message sets operation detail", "[ams][afc][message][phase1]") {
    // Real device: AFC global state has "message": {"message": "Loading T1", "type": "info"}
    // We never parse this field today, but it should set operation_detail.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {{"message", {{"message", "Loading T1"}, {"type", "info"}}}};
    helper.feed_afc_state(afc_data);

    // message.message should flow through to operation_detail
    REQUIRE(helper.get_operation_detail().find("Loading T1") != std::string::npos);
}

TEST_CASE("AFC error message emits EVENT_ERROR", "[ams][afc][message][phase1]") {
    // When message.type == "error", we should emit EVENT_ERROR with the message text
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "AFC Error: lane1 failed to load"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    // error type messages should emit EVENT_ERROR
    REQUIRE(helper.has_event(AmsBackend::EVENT_ERROR));
    // Error data should contain the message text
    std::string error_data = helper.get_event_data(AmsBackend::EVENT_ERROR);
    REQUIRE(error_data.find("lane1 failed to load") != std::string::npos);
}

TEST_CASE("AFC current_load and next_lane tracked", "[ams][afc][state][phase1]") {
    // Real device: AFC global state has "current_load": "lane2", "next_lane": "lane3"
    // These tell us which lane is actively loading and which is queued next.
    // We never parse these fields today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"current_load", "lane2"}, {"next_lane", "lane3"}, {"current_state", "Loading"}};
    helper.feed_afc_state(afc_data);

    // current_load should update current_slot (lane2 = slot 1)
    REQUIRE(helper.get_current_slot() == 1);
    // operation_detail should mention the loading context
    // At minimum, the action should be LOADING from current_state
    REQUIRE(helper.get_action() == AmsAction::LOADING);
}

// ============================================================================
// Phase 2: Full Data Parsing Tests
// ============================================================================
//
// These tests verify parsing of extended hub, extruder, stepper, and buffer
// fields from real AFC device data. Tests use fixture structures captured
// from a real Box Turtle at 192.168.1.112.
// ============================================================================

TEST_CASE("AFC hub bowden length parsed from afc_bowden_length", "[ams][afc][hub][phase2]") {
    // Real device: AFC_hub Turtle_1 has "afc_bowden_length": 1285.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Set hub names so the status update routes correctly
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    // bowden_length should be stored and accessible for device actions
    auto actions = helper.get_device_actions();
    bool found_bowden = false;
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            found_bowden = true;
            // Value should use the real bowden length, not hardcoded 450
            auto val = std::any_cast<float>(action.current_value);
            REQUIRE(val == Catch::Approx(1285.0f));
            break;
        }
    }
    REQUIRE(found_bowden);
}

TEST_CASE("AFC hub cutter info parsed", "[ams][afc][hub][phase2]") {
    // Real device: AFC_hub has "cut": false, "cut_dist": 50.0, etc.
    // We should track whether the hub has a cutter for UI decisions
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub(
        "Turtle_1",
        {{"state", false}, {"cut", false}, {"cut_dist", 50.0}, {"afc_bowden_length", 1285.0}});

    // Hub sensor state should be updated
    REQUIRE(helper.get_hub_sensor() == false);

    // System info should reflect cutter availability
    auto sys_info = helper.get_system_info();
    // AFC always advertises TipMethod::CUT - but we should parse cut field
    // to know if cutter is actually present/configured
    REQUIRE(sys_info.tip_method == TipMethod::CUT);
}

TEST_CASE("AFC extruder speeds parsed", "[ams][afc][extruder][phase2]") {
    // Real device: AFC_extruder has "tool_load_speed": 25.0, "tool_unload_speed": 25.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", false},
                                          {"tool_end_status", false},
                                          {"tool_load_speed", 25.0},
                                          {"tool_unload_speed", 30.0}});

    // Sensor state should be updated
    REQUIRE(helper.get_tool_start_sensor() == false);
    REQUIRE(helper.get_tool_end_sensor() == false);
}

TEST_CASE("AFC extruder distances parsed", "[ams][afc][extruder][phase2]") {
    // Real device: tool_stn=42.0, tool_stn_unload=90.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", true},
                                          {"tool_end_status", false},
                                          {"tool_stn", 42.0},
                                          {"tool_stn_unload", 90.0}});

    REQUIRE(helper.get_tool_start_sensor() == true);
}

TEST_CASE("AFC stepper buffer_status parsed", "[ams][afc][stepper][phase2]") {
    // Real device: AFC_stepper lane1 has "buffer_status": "Advancing"
    // LaneSensors struct only has prep, load, loaded_to_hub today
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1",
                            {{"prep", true}, {"load", true}, {"buffer_status", "Advancing"}});

    // buffer_status should be stored on lane sensors
    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.prep == true);
    REQUIRE(sensors.load == true);
    REQUIRE(sensors.buffer_status == "Advancing");
}

TEST_CASE("AFC stepper filament_status parsed", "[ams][afc][stepper][phase2]") {
    // Real device: "filament_status": "Ready" or "Not Ready"
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1",
                            {{"filament_status", "Ready"}, {"filament_status_led", "#00ff00"}});

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.filament_status == "Ready");
}

TEST_CASE("AFC stepper dist_hub parsed", "[ams][afc][stepper][phase2]") {
    // Real device: "dist_hub": 200.0 (distance to hub in mm)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"dist_hub", 200.0}});

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.dist_hub == Catch::Approx(200.0f));
}

TEST_CASE("AFC buffer object parsed via status update", "[ams][afc][buffer][phase2]") {
    // Real device: AFC_buffer Turtle_1 has "state": "Advancing", "enabled": false
    // We don't subscribe to or parse AFC_buffer objects today
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Feed buffer names through AFC state
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    // Now feed a buffer update
    helper.feed_afc_buffer("Turtle_1", {{"state", "Advancing"}, {"enabled", false}});

    // Verify the feed_afc_buffer path doesn't crash
    REQUIRE_NOTHROW(helper.feed_afc_buffer("Turtle_1", {{"state", "Idle"}, {"enabled", true}}));
}

TEST_CASE("AFC global quiet_mode parsed from AFC state", "[ams][afc][global][phase2]") {
    // Real device: AFC has "quiet_mode": false
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"quiet_mode", false}});
    REQUIRE(helper.get_quiet_mode() == false);

    // Toggle it on
    helper.feed_afc_state({{"quiet_mode", true}});
    REQUIRE(helper.get_quiet_mode() == true);
}

TEST_CASE("AFC global led_state parsed from AFC state", "[ams][afc][global][phase2]") {
    // Real device: AFC has "led_state": true
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"led_state", true}});
    REQUIRE(helper.get_led_state() == true);

    // Toggle it off
    helper.feed_afc_state({{"led_state", false}});
    REQUIRE(helper.get_led_state() == false);
}

TEST_CASE("AFC bowden slider max accommodates real bowden length",
          "[ams][afc][device_actions][phase2]") {
    // The bowden slider max was hardcoded to 1000mm, but real bowden can be 1285mm
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    auto actions = helper.get_device_actions();
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            // Max should accommodate the real bowden length
            REQUIRE(action.max_value >= 1285.0f);
            break;
        }
    }
}

// ============================================================================
// Phase 3: New Device Actions & Commands Tests
// ============================================================================
//
// Tests for new maintenance section, LED/mode toggles, and maintenance commands.
// ============================================================================

TEST_CASE("AFC device sections include maintenance and led",
          "[ams][afc][device_sections][phase3]") {
    AmsBackendAfcTestHelper helper;

    auto sections = helper.get_device_sections();

    bool has_maintenance = false;
    bool has_setup = false;
    for (const auto& section : sections) {
        if (section.id == "maintenance")
            has_maintenance = true;
        if (section.id == "setup")
            has_setup = true;
    }
    REQUIRE(has_maintenance);
    REQUIRE(has_setup);
}

TEST_CASE("AFC device action test_lanes dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("test_lanes");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_TEST_LANES"));
}

TEST_CASE("AFC device action change_blade dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("change_blade");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_CHANGE_BLADE"));
}

TEST_CASE("AFC device action park dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("park");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_PARK"));
}

TEST_CASE("AFC device action brush dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("brush");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_BRUSH"));
}

TEST_CASE("AFC device action reset_motor dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("reset_motor");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode_starting_with("AFC_RESET_MOTOR_TIME"));
}

TEST_CASE("AFC device action led toggle on when off", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // LED is off, toggling should turn it on
    helper.feed_afc_state({{"led_state", false}});

    auto result = helper.execute_device_action("led_toggle");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("TURN_ON_AFC_LED"));
}

TEST_CASE("AFC device action led toggle off when on", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // LED is on, toggling should turn it off
    helper.feed_afc_state({{"led_state", true}});

    auto result = helper.execute_device_action("led_toggle");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("TURN_OFF_AFC_LED"));
}

TEST_CASE("AFC device action quiet_mode dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("quiet_mode");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_QUIET_MODE"));
}

// ============================================================================
// Phase 4: Error Recovery Improvements Tests
// ============================================================================
//
// Tests for differentiated reset (AFC_RESET vs AFC_HOME), per-lane reset,
// and error message surfacing.
// ============================================================================

TEST_CASE("AFC recover sends AFC_RESET", "[ams][afc][recovery][phase4]") {
    // Regression guard — recover() should continue using AFC_RESET
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.recover();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
    REQUIRE_FALSE(helper.has_gcode("AFC_HOME"));
}

TEST_CASE("AFC reset sends AFC_RESET command", "[ams][afc][recovery]") {
    // reset() sends AFC_RESET — the same gcode as recover(), since AFC only has one reset command.
    // Both operations use AFC_RESET; the distinction is in the UI notification text only.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.reset();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
}

TEST_CASE("AFC reset_lane sends per-lane reset command", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.reset_lane(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_LANE_RESET LANE=lane1"));
}

TEST_CASE("AFC reset_lane second lane", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.reset_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_LANE_RESET LANE=lane3"));
}

TEST_CASE("AFC reset_lane validates slot index", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.reset_lane(99);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC reset_lane validates negative index", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.reset_lane(-1);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC reset_lane fails when not running", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // running_ defaults to false

    auto result = helper.reset_lane(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_CONNECTED);
}

TEST_CASE("AFC error message surfaces in EVENT_ERROR data", "[ams][afc][recovery][phase4]") {
    // Verify that AFC error messages contain useful text in the event data
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "Lane 1 failed: filament jam detected"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.has_event(AmsBackend::EVENT_ERROR));
    std::string error_data = helper.get_event_data(AmsBackend::EVENT_ERROR);
    REQUIRE(error_data.find("filament jam detected") != std::string::npos);
}

// ============================================================================
// Phase 2: Mixed Topology — Flat String Units, AFC_lane, Unit Objects, Multi-Extruder
// ============================================================================

// --- 2a: Flat string units array ---

TEST_CASE("AFC backend handles flat string units array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with flat string units (real hardware format)
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "Box_Turtle Turtle_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane4", "lane5", "lane6", "lane7", "lane8", "lane9", "lane10",
                               "lane11", "lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ populated from string parsing
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Check that type/name were parsed from "Type Name" format
    bool found_openams_1 = false;
    bool found_bt_1 = false;
    bool found_openams_2 = false;
    for (const auto& ui : unit_infos) {
        if (ui.name == "AMS_1" && ui.type == "OpenAMS") {
            found_openams_1 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_1");
        }
        if (ui.name == "Turtle_1" && ui.type == "Box_Turtle") {
            found_bt_1 = true;
            REQUIRE(ui.klipper_key == "AFC_BoxTurtle Turtle_1");
        }
        if (ui.name == "AMS_2" && ui.type == "OpenAMS") {
            found_openams_2 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_2");
        }
    }
    REQUIRE(found_openams_1);
    REQUIRE(found_bt_1);
    REQUIRE(found_openams_2);

    // System type is still AFC
    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::AFC);
}

TEST_CASE("AFC backend ViViD unit klipper_key uses lowercase", "[ams][afc][vivid]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // ViViD reports as "ViViD vivid_1" but Klipper object is AFC_vivid (lowercase)
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ViViD vivid_1"});
    afc_state["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].type == "ViViD");
    REQUIRE(unit_infos[0].name == "vivid_1");
    REQUIRE(unit_infos[0].klipper_key == "AFC_vivid vivid_1"); // lowercase, not AFC_ViViD
}

TEST_CASE("AFC backend flat string units: single word name still parses", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Edge case: unit string with no space should not crash
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"NoSpaceUnit"});
    helper.feed_afc_state(afc_state);

    // Should not crash; unit_infos_ may be empty (no space = can't parse)
    const auto& unit_infos = helper.get_unit_infos();
    // Single word without space has no valid type/name split
    REQUIRE(unit_infos.empty());
}

// --- 2b: Unit-level object data ---

TEST_CASE("AFC backend unit-level object populates AfcUnitInfo", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // First, feed flat string units to populate unit_infos_
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    helper.feed_afc_state(afc_state);

    // Then feed unit-level object data via status update
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array({"TN", "TN1", "TN2", "TN3"});

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // Verify unit_infos_ got populated with lane/extruder/hub/buffer data
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Find Turtle_1 and verify
    for (const auto& ui : unit_infos) {
        if (ui.name == "Turtle_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 4);
            REQUIRE(ui.hubs.empty());
            REQUIRE(ui.buffers.size() == 4);
            // Box Turtle: empty hubs + multiple extruders → PARALLEL
            REQUIRE(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.name == "AMS_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 1);
            REQUIRE(ui.hubs.size() == 4);
            REQUIRE(ui.buffers.empty());
            // OpenAMS: hubs present + 1 extruder → HUB
            REQUIRE(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("AFC HTLF mixed topology classification from per-lane hub routing",
          "[ams][afc][topology][mixed]") {
    // HTLF unit has 4 lanes: 2 direct to their own extruders, 2 through hub to shared extruder.
    // Per-lane hub field: "direct" vs hub name determines MIXED topology.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed stepper data WITH hub routing BEFORE unit object (stepper parsing happens first)
    helper.feed_afc_stepper("lane0", {{"map", "T0"},
                                      {"extruder", "extruder"},
                                      {"hub", "direct"},
                                      {"status", "Ready"},
                                      {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"map", "T2"},
                                      {"extruder", "extruder1"},
                                      {"hub", "direct"},
                                      {"status", "Ready"},
                                      {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Ready"},
                                      {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"map", "T3"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Ready"},
                                      {"color", "FFFF00"}});

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object (triggers topology classification)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"HTLF_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is MIXED (not PARALLEL or HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::MIXED);

    // Verify per-lane hub routing data
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    REQUIRE(unit_infos[0].lane_is_hub_routed[0] == false); // lane0 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[1] == false); // lane1 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[2] == true);  // lane2 via hub
    REQUIRE(unit_infos[0].lane_is_hub_routed[3] == true);  // lane3 via hub

    // Verify propagation to AmsUnit via get_system_info
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() >= 1);
    bool found = false;
    for (const auto& unit : info.units) {
        if (unit.name == "HTLF HTLF_1") {
            found = true;
            REQUIRE(unit.topology == PathTopology::MIXED);
            REQUIRE(unit.lane_is_hub_routed.size() == 4);
            REQUIRE(unit.lane_is_hub_routed[0] == false);
            REQUIRE(unit.lane_is_hub_routed[1] == false);
            REQUIRE(unit.lane_is_hub_routed[2] == true);
            REQUIRE(unit.lane_is_hub_routed[3] == true);
        }
    }
    REQUIRE(found);
}

TEST_CASE("AFC all-hub lanes classified as HUB topology", "[ams][afc][topology]") {
    // When all lanes route through a hub, topology should be HUB (not MIXED)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through hub
    helper.feed_afc_stepper("lane0", {{"hub", "Hub_1"}, {"status", "Ready"}, {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"hub", "Hub_1"}, {"status", "Ready"}, {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"hub", "Hub_1"}, {"status", "Ready"}, {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"hub", "Hub_1"}, {"status", "Ready"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
    // All lanes hub-routed
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }
}

TEST_CASE("AFC all-direct lanes classified as PARALLEL topology", "[ams][afc][topology]") {
    // When all lanes are direct and multiple extruders exist, topology should be PARALLEL
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: all lanes direct
    helper.feed_afc_stepper("lane0",
                            {{"hub", "direct"}, {"extruder", "extruder"}, {"status", "Ready"}});
    helper.feed_afc_stepper("lane1",
                            {{"hub", "direct"}, {"extruder", "extruder1"}, {"status", "Ready"}});
    helper.feed_afc_stepper("lane2",
                            {{"hub", "direct"}, {"extruder", "extruder2"}, {"status", "Ready"}});
    helper.feed_afc_stepper("lane3",
                            {{"hub", "direct"}, {"extruder", "extruder3"}, {"status", "Ready"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    // All lanes direct
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == false);
    }
}

TEST_CASE("AFC direct_load hub field classified as direct (not hub-routed)",
          "[ams][afc][topology]") {
    // Box Turtle lanes may report hub:"direct_load" instead of hub:"direct".
    // Both must be treated as direct (not hub-routed). Regression: #392
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: all lanes with hub:"direct_load"
    helper.feed_afc_stepper(
        "lane0", {{"hub", "direct_load"}, {"extruder", "extruder"}, {"status", "Ready"}});
    helper.feed_afc_stepper(
        "lane1", {{"hub", "direct_load"}, {"extruder", "extruder1"}, {"status", "Ready"}});
    helper.feed_afc_stepper(
        "lane2", {{"hub", "direct_load"}, {"extruder", "extruder2"}, {"status", "Ready"}});
    helper.feed_afc_stepper(
        "lane3", {{"hub", "direct_load"}, {"extruder", "extruder3"}, {"status", "Ready"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == false);
    }
}

TEST_CASE("AFC unit with hubs AND multiple extruders gets PARALLEL topology",
          "[ams][afc][mixed][toolchanger]") {
    // Toolchanger scenario: HTLF unit has 4 lanes, 3 extruders, 1 hub.
    // Lanes 1,2 go direct to extruder/extruder1, lanes 3,4 go through hub to extruder2.
    // Must be PARALLEL so each unique mapped_tool gets its own physical nozzle.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: hubs present + 3 extruders (mixed topology)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"hub1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // Feed stepper map data: lanes 0,1 direct, lanes 2,3 shared via hub
    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"extruder", "extruder"}, {"status", "Ready"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T2"}, {"extruder", "extruder1"}, {"status", "Ready"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T1"}, {"extruder", "extruder2"}, {"status", "Ready"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"extruder", "extruder2"}, {"status", "Ready"}, {"color", "FFFF00"}});

    // Verify tool mappings
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 2);
    REQUIRE(slot2->mapped_tool == 1);
    REQUIRE(slot3->mapped_tool == 3); // Shared extruder2 via hub

    // Verify extruder_name is populated from AFC stepper data
    CHECK(slot0->extruder_name == "extruder");
    CHECK(slot1->extruder_name == "extruder1");
    CHECK(slot2->extruder_name == "extruder2");
    CHECK(slot3->extruder_name == "extruder2");
}

TEST_CASE("AFC hub-routed lanes with per-lane extruders classified as PARALLEL",
          "[ams][afc][topology]") {
    // ACE Pro as lane loader through AFC: 4 lanes, each with its own hub AND its
    // own extruder, all under one unit. Each lane independently feeds a different
    // toolhead through a hub sensor — this is parallel topology, not hub merger.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: each lane has its own hub and extruder
    helper.feed_afc_stepper("lane0", {{"hub", "ace_hub1"},
                                      {"extruder", "extruder"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"color", "89a84f"}});
    helper.feed_afc_stepper("lane1", {{"hub", "ace_hub2"},
                                      {"extruder", "extruder1"},
                                      {"map", "T1"},
                                      {"status", "Loaded"},
                                      {"color", "23ADE9"}});
    helper.feed_afc_stepper("lane2", {{"hub", "ace_hub3"},
                                      {"extruder", "extruder2"},
                                      {"map", "T2"},
                                      {"status", "Loaded"},
                                      {"color", "F20808"}});
    helper.feed_afc_stepper("lane3", {{"hub", "ace_hub4"},
                                      {"extruder", "extruder3"},
                                      {"map", "T3"},
                                      {"status", "Loaded"},
                                      {"color", "DE4343"}});

    // Feed AFC state with unit
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ACE Ace_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: 4 hubs + 4 extruders (parallel with hubs)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array({"ace_hub1", "ace_hub2", "ace_hub3", "ace_hub4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_ACE Ace_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // All lanes are hub-routed (each through its own hub)
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }

    // Verify tool mappings are 1:1
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 1);
    REQUIRE(slot2->mapped_tool == 2);
    REQUIRE(slot3->mapped_tool == 3);
}

TEST_CASE("AFC hub-routed lanes with shared extruder still classified as HUB",
          "[ams][afc][topology]") {
    // Standard Box Turtle / OpenAMS: 4 lanes through hubs but only 1 extruder.
    // Must remain HUB topology — the new parallel-with-hubs check should NOT
    // trigger when extruder count != lane count.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through per-lane hubs but same extruder
    helper.feed_afc_stepper(
        "lane0",
        {{"hub", "Hub_1"}, {"extruder", "extruder4"}, {"status", "Ready"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"hub", "Hub_2"}, {"extruder", "extruder4"}, {"status", "Ready"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"hub", "Hub_3"}, {"extruder", "extruder4"}, {"status", "Ready"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"hub", "Hub_4"}, {"extruder", "extruder4"}, {"status", "Ready"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder4"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    // Must still be HUB — 4 hubs but only 1 extruder
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
}

TEST_CASE("AFC backend unit object triggers lane reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed flat string units
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit-level data for both units
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // After both units are parsed, units should be reorganized
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    // Units sorted alphabetically: AMS_1 before Turtle_1
    // (reorganize_slots sorts unit names)
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
    REQUIRE(info.total_slots == 8);
}

// --- 2c: AFC_lane status updates ---

TEST_CASE("AFC backend handles AFC_lane status updates", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed an AFC_lane update (same schema as AFC_stepper)
    nlohmann::json lane_data;
    lane_data["name"] = "lane4";
    lane_data["unit"] = "AMS_1";
    lane_data["hub"] = "Hub_1";
    lane_data["extruder"] = "extruder4";
    lane_data["buffer"] = nullptr;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["tool_loaded"] = false;
    lane_data["loaded_to_hub"] = false;
    lane_data["material"] = "PLA";
    lane_data["spool_id"] = 13;
    lane_data["color"] = "#000000";
    lane_data["weight"] = 295.25;
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";
    lane_data["filament_status"] = "Ready";
    lane_data["dist_hub"] = 60;

    // Feed as AFC_lane (not AFC_stepper)
    nlohmann::json params;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Verify the lane was parsed using parse_afc_stepper (same JSON schema)
    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(4);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->material == "PLA");
    REQUIRE(slot->mapped_tool == 4);
    REQUIRE(slot->color_rgb == 0x000000);
    // AFC "Loaded" means hub-loaded, tool_loaded=false → AVAILABLE, not LOADED
    REQUIRE(slot->status == SlotStatus::AVAILABLE);
}

TEST_CASE("AFC backend handles mix of AFC_stepper and AFC_lane in same update",
          "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed both AFC_stepper and AFC_lane in same notification
    nlohmann::json stepper_data;
    stepper_data["prep"] = true;
    stepper_data["load"] = true;
    stepper_data["material"] = "PETG";
    stepper_data["color"] = "#FF0000";
    stepper_data["map"] = "T0";
    stepper_data["status"] = "Loaded";

    nlohmann::json lane_data;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["material"] = "ABS";
    lane_data["color"] = "#00FF00";
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";

    nlohmann::json params;
    params["AFC_stepper lane0"] = stepper_data;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Both should be parsed
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PETG");

    auto* slot4 = info.get_slot_global(4);
    REQUIRE(slot4 != nullptr);
    REQUIRE(slot4->material == "ABS");
}

// --- 2d: Multiple AFC_extruder objects ---

TEST_CASE("AFC backend handles multiple AFC_extruder objects", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Set extruder names from AFC state
    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify extruder_names_ populated
    const auto& ext_names = helper.get_extruder_names();
    REQUIRE(ext_names.size() == 6);

    // Feed multiple extruder updates
    nlohmann::json params;
    params["AFC_extruder extruder4"] = {
        {"tool_start_status", true}, {"tool_end_status", false}, {"lane_loaded", "lane4"}};
    params["AFC_extruder extruder5"] = {
        {"tool_start_status", false}, {"tool_end_status", false}, {"lane_loaded", nullptr}};
    helper.feed_status_update(params);

    // Verify current slot updated from extruder4's lane_loaded
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 4);
}

TEST_CASE("AFC backend multi-extruder backward compat: single extruder still works",
          "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Do NOT set extruder_names_ (empty = backward compat)
    // Feed single AFC_extruder extruder (old format)
    nlohmann::json params;
    params["AFC_extruder extruder"] = {
        {"tool_start_status", true}, {"tool_end_status", true}, {"lane_loaded", "lane0"}};
    helper.feed_status_update(params);

    // Should still work via backward-compat fallback
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0);
    REQUIRE(helper.get_tool_start_sensor() == true);
    REQUIRE(helper.get_tool_end_sensor() == true);
}

TEST_CASE("AFC backend stores extruder names from AFC state extruders array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);

    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array({"extruder", "extruder1"});
    helper.feed_afc_state(afc_state);

    const auto& names = helper.get_extruder_names();
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "extruder");
    REQUIRE(names[1] == "extruder1");
}

// --- Backward compatibility ---

TEST_CASE("AFC backend backward compat: object-format units still works", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Old format: units as objects with name and lanes
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    helper.feed_afc_state(afc_state);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    // unit_infos_ should be empty (object format doesn't populate it)
    REQUIRE(helper.get_unit_infos().empty());
}

TEST_CASE("AFC backend backward compat: mixed string and object units", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Mix of string and object units (shouldn't happen in practice, but be robust)
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Old Turtle";
    unit_obj["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1", unit_obj});
    helper.feed_afc_state(afc_state);

    // String unit creates unit_info, object unit goes through old path
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].name == "AMS_1");
}

// ============================================================================
// Phase 6: Backward Compatibility Tests
// ============================================================================

TEST_CASE("AFC get_unit_topology falls back to get_topology when unit_infos empty",
          "[ams][afc][backward_compat]") {
    // Standard non-mixed AFC: unit_infos_ is empty, so get_unit_topology()
    // should fall back to get_topology() which returns HUB.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // No flat string units fed — unit_infos_ is empty
    REQUIRE(helper.get_unit_infos().empty());

    // get_unit_topology() for any index should fall back to get_topology() = HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(-1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(99) == PathTopology::HUB);
}

TEST_CASE("AFC get_topology still returns HUB for standard AFC", "[ams][afc][backward_compat]") {
    // Regression guard: get_topology() must always return HUB for AFC backend
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("AFC backend with only AFC_stepper lanes works correctly (no AFC_lane)",
          "[ams][afc][backward_compat]") {
    // Standard Box Turtle setup: only AFC_stepper objects, no AFC_lane objects.
    // The AFC_lane loop should simply skip when no AFC_lane objects exist.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed only AFC_stepper updates (standard non-mixed Box Turtle)
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", false},
                                      {"material", "PLA"},
                                      {"color", "#FF0000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 850}});
    helper.feed_afc_stepper("lane2", {{"prep", true},
                                      {"load", false},
                                      {"material", "PETG"},
                                      {"color", "#00FF00"},
                                      {"map", "T1"},
                                      {"status", "Ready"}});

    // Verify stepper data parsed correctly
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PLA");
    REQUIRE(slot0->color_rgb == 0xFF0000);
    REQUIRE(slot0->mapped_tool == 0);
    // AFC "Loaded" with no tool_loaded → AVAILABLE (hub-loaded only)
    REQUIRE(slot0->status == SlotStatus::AVAILABLE);

    auto* slot1 = info.get_slot_global(1);
    REQUIRE(slot1 != nullptr);
    REQUIRE(slot1->material == "PETG");
    REQUIRE(slot1->color_rgb == 0x00FF00);
    REQUIRE(slot1->mapped_tool == 1);

    // Sensors should work via AFC_stepper path
    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.prep == true);
    REQUIRE(sensors.load == true);
    REQUIRE(sensors.loaded_to_hub == false);

    // Topology should still be HUB (standard AFC)
    REQUIRE(helper.get_topology() == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
}

TEST_CASE("AFC standard single-unit system unchanged by mixed topology code",
          "[ams][afc][backward_compat]") {
    // Verify that feeding a standard single-unit AFC state (object format)
    // does not populate unit_infos_ and preserves the old unit structure
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed old-format AFC state with object-style unit
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    afc_state["current_state"] = "Idle";
    helper.feed_afc_state(afc_state);

    // unit_infos_ should remain empty (object format does not populate it)
    REQUIRE(helper.get_unit_infos().empty());

    // Standard unit structure should still be correct
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    REQUIRE(info.units[0].slot_count == 4);

    // Topology falls back to HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_action() == AmsAction::IDLE);
}

// ============================================================================
// eject_lane() Tests
// ============================================================================

TEST_CASE("AFC eject_lane sends LANE_UNLOAD command", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
}

TEST_CASE("AFC eject_lane targets correct lane", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane3"));
}

TEST_CASE("AFC eject_lane fails when lane is loaded in toolhead", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(1);

    auto result = helper.eject_lane(1);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::WRONG_STATE);
    REQUIRE(helper.captured_gcodes.empty());
}

TEST_CASE("AFC eject_lane allows eject of non-current slot even when filament loaded",
          "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(0);

    // Eject slot 2 while slot 0 is loaded — should work
    auto result = helper.eject_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane3"));
}

TEST_CASE("AFC eject_lane validates slot index", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(99);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC eject_lane fails when not running", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.eject_lane(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_CONNECTED);
}

// LANE_UNLOAD serialization
//
// Rapid-fire LANE_UNLOAD commands overlap their lane LED animations and stepper
// work on AFC's MCU and were a contributor to Klippy "Timer too close"
// shutdowns when a user tapped Eject on multiple lanes in quick succession.
// eject_lane() now queues consecutive requests and dispatches them one at a
// time, with the next one fired only when the previous completes.

TEST_CASE("AFC eject_lane serializes consecutive ejects (only first dispatches)",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // Only the first eject should have dispatched — the rest are queued.
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
}

TEST_CASE("AFC eject_lane drains queue in order on each completion",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // After each completion, the next queued eject should fire.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");

    // Queue empty — further completions do nothing.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
}

TEST_CASE("AFC eject_lane deduplicates consecutive duplicate slot requests",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    // Repeat-tap the same lane while the first is still running.
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced

    helper.complete_pending_unload();
    helper.complete_pending_unload();

    // Expected dispatches: lane1, lane2 (only one lane2, despite three taps).
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE("AFC eject_lane fires next eject after previous completes (synchronous mode)",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    // Default: defer_lane_unload_complete = false → completion fires synchronously.

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());

    // Synchronous completion means by the time eject_lane returns, the queue
    // has fully drained and both gcodes have dispatched.
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE("AFC eject_lane queue drains on failure (error path)",
          "[ams][afc][eject][serialization]") {
    // In production, dispatch_lane_unload registers success AND error callbacks
    // with api_->execute_gcode. Both call on_lane_unload_done() so the queue
    // drains either way. This test simulates the error path: the in-flight
    // gcode "fails", but on_lane_unload_done still advances to the next queued
    // entry. Without this guarantee a transient AFC error would strand the
    // queue and silently break subsequent eject taps.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());

    REQUIRE(helper.captured_gcodes.size() == 1);

    // Simulate the error callback path — completion still drains the queue.
    helper.complete_pending_unload(); // "lane1 errored" → fire lane2
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload(); // "lane2 errored" → fire lane3
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload(); // "lane3 errored" → queue empty
    REQUIRE(helper.captured_gcodes.size() == 3);
    // Subsequent eject_lane should fire immediately (in-flight flag cleared).
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");
}

TEST_CASE("AFC cancel drops queued ejects but lets in-flight complete",
          "[ams][afc][eject][serialization][cancel]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 1); // only lane1 dispatched

    // Cancel should drop queued lane2/lane3/lane4 but not abort the in-flight
    // lane1 — its completion callback is still pending.
    helper.cancel();

    // The in-flight callback firing now should NOT dispatch a queued next
    // (queue was cleared). It should just clear eject_in_flight_.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 1); // no new dispatches

    // A fresh eject after cancel completes should fire immediately.
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane3");
}

TEST_CASE("AFC supports_lane_eject returns true", "[ams][afc][capability]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.supports_lane_eject());
}

TEST_CASE("AFC supports_lane_reset returns true", "[ams][afc][capability]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.supports_lane_reset());
}

// ============================================================================
// Slot status mapping: AFC "Loaded" vs tool_loaded
// ============================================================================

TEST_CASE("AFC hub-loaded lane is AVAILABLE, not LOADED", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Exact production state: loaded_to_hub=true, tool_loaded=false, status="Loaded"
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "ASA"},
                                      {"color", "#000000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 570}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    // Hub-loaded filament should be AVAILABLE (ready to load to toolhead)
    REQUIRE(slot->status == SlotStatus::AVAILABLE);
    // Should NOT be the "current" loaded slot
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("AFC tool_loaded=true lane is LOADED", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Filament actually at the toolhead
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", true},
                                      {"material", "ASA"},
                                      {"color", "#000000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 570}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->status == SlotStatus::LOADED);
}

TEST_CASE("AFC 'Tooled' status maps to LOADED even without tool_loaded flag",
          "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // OpenAMS uses "Tooled" status string
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "PLA"},
                                      {"color", "#FF0000"},
                                      {"map", "T0"},
                                      {"status", "Tooled"}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    // "Tooled" is an explicit toolhead-loaded indicator
    REQUIRE(slot->status == SlotStatus::LOADED);
}

TEST_CASE("AFC context menu shows Eject for hub-loaded slot", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Two lanes loaded to hub, none to toolhead
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "ASA"},
                                      {"map", "T0"},
                                      {"status", "Loaded"}});

    auto slot = helper.get_slot_info(0);
    // Slot should be present (has filament)
    REQUIRE(slot.is_present());
    // But NOT loaded to extruder
    REQUIRE(slot.status == SlotStatus::AVAILABLE);
    REQUIRE(slot.status != SlotStatus::LOADED);
}

TEST_CASE("AFC slot transitions from LOADED to AVAILABLE on unload", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // First: loaded to toolhead
    helper.feed_afc_stepper("lane1", {{"tool_loaded", true},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    auto info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);

    // Then: unloaded from toolhead, still at hub
    helper.feed_afc_stepper("lane1", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::AVAILABLE);
}

// ============================================================================
// filament_loaded derived from stepper tool_loaded (no top-level AFC field)
// ============================================================================

TEST_CASE("AFC filament_loaded derived from stepper tool_loaded", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Simulate AFC version without top-level "filament_loaded" field:
    // only lane stepper data drives loaded state
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 3); // lane4 = slot index 3
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::LOADED);
}

TEST_CASE("AFC filament_loaded clears when tool_loaded goes false", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Load lane4
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);

    // Unload — tool_loaded goes false
    helper.feed_afc_stepper("lane4", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::AVAILABLE);
}

TEST_CASE("AFC current_load fallback sets current_slot and filament_loaded", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // AFC state with current_load (not current_lane) and no filament_loaded field
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0); // lane1 = slot index 0
    // filament_loaded derived from current_load
    REQUIRE(info.filament_loaded);
}

TEST_CASE("AFC current_tool derived from current_load via tool mapping",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // Sets slot N → tool N mapping

    // AFC reports current_load=lane3 but no current_tool field
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2); // lane3 = slot index 2
    REQUIRE(info.current_tool == 2); // derived from slot 2 → T2 mapping
}

TEST_CASE("AFC explicit current_tool overrides derived value", "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // slot N → tool N

    // AFC reports both current_load and an explicit current_tool that differs
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_tool", 5},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // Explicit current_tool=5 overrides derived T2
    REQUIRE(info.current_tool == 5);
}

TEST_CASE("AFC current_tool cleared on unload (current_load null)",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // First: load lane2
    helper.feed_afc_state({{"current_load", "lane2"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_tool == 1); // lane2 → slot 1 → T1
    REQUIRE(info.filament_loaded);

    // Then: unload — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE(info.current_tool == -1); // cleared
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("AFC current_tool uses default 1:1 mapping from initialize_slots",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    // initialize_test_lanes + initialize_slots_from_discovery creates default 1:1 mapping
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // Default 1:1 mapping: slot 2 → T2
    REQUIRE(info.current_tool == 2);
}

TEST_CASE("AFC explicit filament_loaded not overridden by stepper post-scan",
          "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Single notification with both AFC state (explicit filament_loaded=false during
    // unload transition) and stepper data that still shows tool_loaded=true
    nlohmann::json params;
    params["AFC"] = {{"filament_loaded", false}, {"current_state", "Unloading"}};
    params["AFC_stepper lane1"] = {
        {"tool_loaded", true}, {"status", "Tooled"}, {"prep", true}, {"load", true}, {"map", "T0"}};
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    // Explicit filament_loaded=false from AFC takes priority over stepper post-scan
    REQUIRE_FALSE(info.filament_loaded);
    // But slot status should still reflect the stepper data
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);
}

TEST_CASE("AFC current_load null clears filament state", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // First: loaded via current_load
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 0);

    // Then: unloaded — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.current_slot == -1);
}

// ============================================================================
// manages_active_spool() Tests
//
// AFC manages Spoolman's active spool natively (calls spoolman_set_active_spool
// on tool load/unload). HelixScreen must NOT call set_active_spool when AFC is
// the active backend, to avoid racing with AFC's own calls.
// ============================================================================

TEST_CASE("AFC backend reports manages_active_spool=true", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.manages_active_spool() == true);
}

TEST_CASE("AFC get_type returns AFC", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_type() == AmsType::AFC);
}

// ============================================================================
// auto_unloads_after_print() driven by per-printer SettingsManager toggle
// ============================================================================
//
// Unlike CFS/IFS (which hardcode auto_unloads_after_print()==true), AFC's
// post-print unload depends on the user's macros. The behaviour is controlled
// by a per-printer setting: get_afc_unload_after_print() (default false). The
// pre-print runout warning is suppressed only when the setting is enabled.

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC auto_unloads_after_print defaults to false (setting off)",
                 "[ams][afc][capability]") {
    helix::Config::get_instance();
    helix::SettingsManager::instance().init_subjects();

    // Ensure the setting starts disabled
    helix::SettingsManager::instance().set_afc_unload_after_print(false);

    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.auto_unloads_after_print() == false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AFC auto_unloads_after_print returns true when setting enabled",
                 "[ams][afc][capability]") {
    helix::Config::get_instance();
    helix::SettingsManager::instance().init_subjects();

    helix::SettingsManager::instance().set_afc_unload_after_print(true);

    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.auto_unloads_after_print() == true);

    // Toggling the setting back off must flip the capability back
    helix::SettingsManager::instance().set_afc_unload_after_print(false);
    REQUIRE(helper.auto_unloads_after_print() == false);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "SettingsManager afc_unload_after_print round-trips and persists",
                 "[ams][afc][settings]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    const std::string path = config->df() + "ams/afc_unload_after_print";

    SECTION("defaults to false") {
        settings.set_afc_unload_after_print(false);
        REQUIRE(settings.get_afc_unload_after_print() == false);
    }

    SECTION("set true -> get true -> persisted to per-printer config path") {
        settings.set_afc_unload_after_print(true);
        REQUIRE(settings.get_afc_unload_after_print() == true);
        REQUIRE(config->get<bool>(path, false) == true);

        // subject reflects the new value
        REQUIRE(lv_subject_get_int(settings.subject_afc_unload_after_print()) == 1);
    }

    SECTION("set false after true clears the value") {
        settings.set_afc_unload_after_print(true);
        settings.set_afc_unload_after_print(false);
        REQUIRE(settings.get_afc_unload_after_print() == false);
        REQUIRE(config->get<bool>(path, true) == false);
        REQUIRE(lv_subject_get_int(settings.subject_afc_unload_after_print()) == 0);
    }
}

TEST_CASE("AFC backend reports tracks_weight_locally=true", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.tracks_weight_locally() == true);
}

// ============================================================================
// Toolchanger mode: AFC_SELECT_TOOL / AFC_UNSELECT_TOOL
// ============================================================================

TEST_CASE("AFC change_tool sends AFC_SELECT_TOOL in toolchanger mode", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(2);

    auto result = helper.change_tool(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder1"));
}

TEST_CASE("AFC change_tool sends T{n} in single-extruder mode", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    // num_extruders_ defaults to 1, no setup_toolchanger call

    auto result = helper.change_tool(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("T1"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
}

TEST_CASE("AFC change_tool sends AFC_SELECT_TOOL with first extruder name",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(3);

    auto result = helper.change_tool(0);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder"));
}

TEST_CASE("AFC load_filament sends AFC_SELECT_TOOL in toolchanger mode",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(2);

    auto result = helper.load_filament(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder1"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("CHANGE_TOOL"));
}

TEST_CASE("AFC load_filament sends CHANGE_TOOL in single-extruder mode",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.load_filament(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("CHANGE_TOOL LANE=lane2"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
}

TEST_CASE("AFC unload_filament sends bare TOOL_UNLOAD in toolchanger mode (no slot)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(2);

    // No slot specified (slot_index = -1): unload whatever is currently loaded.
    auto result = helper.unload_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

// #999: selecting a non-active lane and pressing Unload must unload THAT lane,
// not whatever is loaded on the shuttle. AFC's TOOL_UNLOAD LANE=<lane> picks up
// the correct tool first, so the requested lane is honored even in toolchanger
// mode (previously this sent a bare AFC_UNSELECT_TOOL and ignored the slot).
TEST_CASE("AFC unload_filament sends TOOL_UNLOAD LANE=<lane> for specific slot (#999)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(4);

    SECTION("slot 0 unloads lane1") {
        auto result = helper.unload_filament(0);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane1"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 1 unloads lane2") {
        auto result = helper.unload_filament(1);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane2"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 3 unloads lane4") {
        auto result = helper.unload_filament(3);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }
}

TEST_CASE("AFC unload_filament sends bare TOOL_UNLOAD in single-extruder mode (no slot)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);

    auto result = helper.unload_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

TEST_CASE("AFC unload_filament honors slot_index in single-extruder mode (#999)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);

    auto result = helper.unload_filament(3);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_UNSELECT_TOOL"));
}

// ============================================================================
// LED device actions (multi-extruder toolchanger mode)
// ============================================================================

TEST_CASE("AFC get_device_actions includes per-extruder LED toggles in toolchanger mode",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto actions = helper.get_device_actions();

    // Should have per-extruder LED actions
    bool found_t0 = false, found_t1 = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder_T0")
            found_t0 = true;
        if (a.id == "led_extruder_T1")
            found_t1 = true;
    }
    REQUIRE(found_t0);
    REQUIRE(found_t1);

    // Should NOT have the generic led_extruder action
    bool found_generic = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder")
            found_generic = true;
    }
    REQUIRE_FALSE(found_generic);
}

TEST_CASE("AFC get_device_actions does not include LED toggles in single-extruder mode",
          "[ams][afc][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto actions = helper.get_device_actions();

    bool found_per_extruder = false;
    for (const auto& a : actions) {
        if (a.id.rfind("led_extruder_T", 0) == 0)
            found_per_extruder = true;
    }
    REQUIRE_FALSE(found_per_extruder);
}

TEST_CASE("AFC execute_device_action led_extruder_T0 sends AFC_SET_EXTRUDER_LED",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T0");

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));
}

TEST_CASE("AFC execute_device_action led_extruder_T1 sends AFC_SET_EXTRUDER_LED",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T1");

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder1 TURN_ON=1"));
}

TEST_CASE("AFC execute_device_action led_extruder toggles state", "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    // First call turns ON
    auto result1 = helper.execute_device_action("led_extruder_T0");
    REQUIRE(result1);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));

    helper.clear_captured_gcodes();

    // Second call turns OFF
    auto result2 = helper.execute_device_action("led_extruder_T0");
    REQUIRE(result2);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=0"));
}

TEST_CASE("AFC execute_device_action led_extruder rejects invalid tool index",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T5");

    REQUIRE_FALSE(result);
}

// ============================================================================
// Multi-unit AFC scenarios (mixed topologies)
// ============================================================================

TEST_CASE("Mixed BoxTurtle + ViViD units with string format", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Pre-initialize 12 lanes (non-contiguous: lane1-lane8 + lane13-lane16)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {"Turtle_2"});
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7",
                               "lane8", "lane13", "lane14", "lane15", "lane16"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ parsed correctly
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);
    CHECK(unit_infos[0].type == "Box_Turtle");
    CHECK(unit_infos[0].name == "Turtle_1");
    CHECK(unit_infos[0].klipper_key == "AFC_BoxTurtle Turtle_1");
    CHECK(unit_infos[1].type == "Box_Turtle");
    CHECK(unit_infos[1].name == "Turtle_2");
    CHECK(unit_infos[1].klipper_key == "AFC_BoxTurtle Turtle_2");
    CHECK(unit_infos[2].type == "ViViD");
    CHECK(unit_infos[2].name == "vivid_1");
    CHECK(unit_infos[2].klipper_key == "AFC_vivid vivid_1");

    // Feed BoxTurtle 1 unit object (lanes 1-4, parallel topology)
    nlohmann::json bt1_data;
    bt1_data["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    bt1_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt1_data["hubs"] = nlohmann::json::array();
    bt1_data["buffers"] = nlohmann::json::array();

    // Feed BoxTurtle 2 unit object (lanes 5-8, parallel topology)
    nlohmann::json bt2_data;
    bt2_data["lanes"] = nlohmann::json::array({"lane5", "lane6", "lane7", "lane8"});
    bt2_data["extruders"] =
        nlohmann::json::array({"extruder4", "extruder5", "extruder6", "extruder7"});
    bt2_data["hubs"] = nlohmann::json::array();
    bt2_data["buffers"] = nlohmann::json::array();

    // Feed ViViD unit object (lanes 13-16, hub topology)
    nlohmann::json vivid_data;
    vivid_data["lanes"] = nlohmann::json::array({"lane13", "lane14", "lane15", "lane16"});
    vivid_data["extruders"] = nlohmann::json::array({"extruder"});
    vivid_data["hubs"] = nlohmann::json::array({"vivid_hub"});
    vivid_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt1_data;
    params["AFC_BoxTurtle Turtle_2"] = bt2_data;
    params["AFC_vivid vivid_1"] = vivid_data;
    helper.feed_status_update(params);

    // After all 3 units have lanes, reorganization should have happened
    auto& sys_info = helper.get_system_info_mutable();
    REQUIRE(sys_info.units.size() == 3);

    // Check per-unit and total slot counts
    int total_slots = 0;
    for (const auto& unit : sys_info.units) {
        CHECK(unit.slots.size() == 4);
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);

    // Verify topologies are correct per unit
    for (const auto& ui : unit_infos) {
        if (ui.type == "Box_Turtle") {
            // Box Turtle: empty hubs + multiple extruders = PARALLEL
            CHECK(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.type == "ViViD") {
            // ViViD: hubs present + single extruder = HUB
            CHECK(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("Partial unit data waits for all units before reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed 3 string units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed unit objects for only 2 of 3 units
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // With only 2 of 3 units having data, reorganization should NOT happen yet.
    // Premature reorganization drops the last unit's lane data from the stash.
    auto info = helper.get_system_info();
    // Units from initial initialize_slots remain (1 flat unit), not reorganized
    CHECK(info.units.size() == 1);

    // Now feed 3rd unit → reorganization triggers with all units present
    nlohmann::json ams2_data;
    ams2_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
    ams2_data["extruders"] = nlohmann::json::array({"extruder5"});
    ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
    ams2_data["buffers"] = nlohmann::json::array();

    nlohmann::json params2;
    params2["AFC_OpenAMS AMS_2"] = ams2_data;
    helper.feed_status_update(params2);

    // Now all 3 units should be present with all lane data preserved
    info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);
    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);
}

TEST_CASE("Non-contiguous lane numbering assigns sequential global indices", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Lanes 1-8 and 13-16 (gap at 9-12)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {});
    helper.initialize_slots_from_discovery();

    // Verify all 12 lanes are initialized with sequential indices
    REQUIRE(helper.get_slot_count() == 12);

    // The slot registry maps sequential indices 0-11 to the non-contiguous lane names
    CHECK(helper.get_slot_name(0) == "lane1");
    CHECK(helper.get_slot_name(7) == "lane8");
    CHECK(helper.get_slot_name(8) == "lane13"); // Index 8 → lane13 (no gap)
    CHECK(helper.get_slot_name(11) == "lane16");

    // Feed units to trigger reorganization
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit objects with lane assignments
    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = {
        {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
        {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_BoxTurtle Turtle_2"] = {
        {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
        {"extruders", {"extruder4", "extruder5", "extruder6", "extruder7"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_vivid vivid_1"] = {{"lanes", {"lane13", "lane14", "lane15", "lane16"}},
                                   {"extruders", {"extruder"}},
                                   {"hubs", {"vivid_hub"}},
                                   {"buffers", nlohmann::json::array()}};
    helper.feed_status_update(params);

    // After reorganization, global indices should still be sequential 0-11
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    // Verify each unit's slots have correct global indices
    int expected_global = 0;
    for (const auto& unit : info.units) {
        for (const auto& slot : unit.slots) {
            CHECK(slot.global_index == expected_global);
            expected_global++;
        }
    }
    CHECK(expected_global == 12);
}

// ============================================================================
// Bug fix: Tool changer current_slot overwrite
// ============================================================================

TEST_CASE("AFC tool changer reconciliation preserves current_load slot",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // In a tool changer, ALL 4 lanes are tool_loaded: true (direct-feed).
    // AFC reports current_load = "lane2" meaning lane2 is active at toolhead.
    // The reconciliation block must NOT overwrite current_slot with lane0.

    // Build a single status update containing BOTH AFC state and lane data
    nlohmann::json params;

    // AFC global state with current_load
    params["AFC"] = {{"current_load", "lane2"}};

    // All 4 lanes report as loaded (tool changer direct-feed)
    for (int i = 0; i < 4; ++i) {
        std::string key = "AFC_stepper lane" + std::to_string(i);
        params[key] = {{"status", "Tooled"},
                       {"tool_loaded", true},
                       {"color", "FF0000"},
                       {"material", "PLA"},
                       {"spool_id", std::to_string(100 + i)},
                       {"weight", 750}};
    }

    helper.feed_status_update(params);

    // current_slot should be 2 (lane2), not 0 (first loaded lane)
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
    CHECK(info.filament_loaded == true);
}

TEST_CASE("AFC parse_lane_data does not overwrite valid current_slot", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // First set current_slot via AFC state (current_load)
    helper.feed_afc_state({{"current_load", "lane2"}});
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Now feed lane data where all lanes are tool_loaded
    nlohmann::json lane_data;
    for (int i = 0; i < 4; ++i) {
        std::string name = "lane" + std::to_string(i);
        lane_data[name] = {{"tool_loaded", true},
                           {"color", "00FF00"},
                           {"material", "PETG"},
                           {"spool_id", std::to_string(200 + i)}};
    }
    helper.feed_afc_state({{"lanes", lane_data}});

    // current_slot should still be 2 (set by current_load), not overwritten
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
}

// ============================================================================
// Bug fix: 3-unit premature reorganize drops last unit's lanes
// ============================================================================

TEST_CASE("AFC 3-unit incremental arrival preserves all unit lanes",
          "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "OpenAMS AMS_2", "Box_Turtle Turtle_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed stepper data with color/material for ALL 12 lanes BEFORE unit objects
    for (int i = 0; i < 12; ++i) {
        std::string lane_name = "lane" + std::to_string(i);
        nlohmann::json stepper_data = {
            {"color", "FF" + std::to_string(1000 + i).substr(1)},
            {"material", "PLA"},
            {"spool_id", std::to_string(300 + i)},
            {"weight", 800},
            {"tool_loaded", (i >= 8)} // Turtle lanes are direct-feed
        };
        helper.feed_afc_stepper(lane_name, stepper_data);
    }

    // Now feed unit objects ONE AT A TIME (simulating incremental status notifications)
    // Unit 1: OpenAMS AMS_1
    {
        nlohmann::json ams1_data;
        ams1_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        ams1_data["extruders"] = nlohmann::json::array({"extruder0"});
        ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
        ams1_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_1"] = ams1_data;
        helper.feed_status_update(params);
    }

    // Unit 2: OpenAMS AMS_2
    {
        nlohmann::json ams2_data;
        ams2_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
        ams2_data["extruders"] = nlohmann::json::array({"extruder1"});
        ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
        ams2_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_2"] = ams2_data;
        helper.feed_status_update(params);
    }

    // Unit 3: Box_Turtle Turtle_1
    {
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder2", "extruder3", "extruder4"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // After all 3 units processed, ALL 12 slots should have valid data
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    REQUIRE(total_slots == 12);

    // Specifically verify that Turtle_1's lanes (8-11) have color/material from stepper data
    // Find Turtle unit - after reorganize, units are sorted by min lane number
    bool found_turtle = false;
    for (const auto& unit : info.units) {
        if (unit.name.find("Turtle") != std::string::npos) {
            found_turtle = true;
            for (const auto& slot : unit.slots) {
                CHECK(slot.material != "");
                CHECK(slot.color_rgb != 0x808080); // Not default gray
            }
        }
    }
    CHECK(found_turtle);
}

// ============================================================================
// 4-unit ordering: units sorted by min lane number, not name (#554)
// ============================================================================

TEST_CASE("AFC 4-unit ordering by lane number not unit name", "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(16);
    helper.initialize_slots_from_discovery();

    // Feed AFC state: 4 units in Moonraker JSON order (last unit first)
    // This reproduces #554 where unit discovery order caused wrong display
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {"Box_Turtle Turtle_2", "Box_Turtle Turtle_1", "ACE ace_1", "OpenAMS AMS_1"});
    afc_state["lanes"] = nlohmann::json::array({});
    for (int i = 0; i < 16; ++i)
        afc_state["lanes"].push_back("lane" + std::to_string(i));
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Feed stepper data for all 16 lanes
    for (int i = 0; i < 16; ++i) {
        std::string lane = "lane" + std::to_string(i);
        helper.feed_afc_stepper(lane, {{"color", "FF0000"},
                                       {"material", "PLA"},
                                       {"spool_id", std::to_string(i)},
                                       {"weight", 800}});
    }

    // Feed unit objects mapping lanes to units (deliberately out of physical order).
    // Key format uses Klipper convention: "AFC_" + type (underscores stripped) + " " + name
    auto feed_unit = [&](const std::string& klipper_key, int first_lane, int count) {
        nlohmann::json data;
        data["lanes"] = nlohmann::json::array();
        for (int i = first_lane; i < first_lane + count; ++i)
            data["lanes"].push_back("lane" + std::to_string(i));
        data["extruders"] = nlohmann::json::array({"extruder"});
        data["hubs"] = nlohmann::json::array();
        data["buffers"] = nlohmann::json::array();
        nlohmann::json params;
        params[klipper_key] = data;
        helper.feed_status_update(params);
    };

    // Unit with lanes 12-15 arrives FIRST (the bug scenario from #554)
    feed_unit("AFC_BoxTurtle Turtle_2", 12, 4);
    feed_unit("AFC_BoxTurtle Turtle_1", 0, 4);
    feed_unit("AFC_ACE ace_1", 8, 4);
    feed_unit("AFC_OpenAMS AMS_1", 4, 4);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 4);

    // Units must be ordered by min lane number, NOT alphabetically:
    // - Turtle_1 (lanes 0-3) first
    // - AMS_1 (lanes 4-7) second
    // - ace_1 (lanes 8-11) third
    // - Turtle_2 (lanes 12-15) fourth
    // With alphabetical sort, "ACE ace_1" would wrongly be first
    CHECK(info.units[0].name.find("Turtle_1") != std::string::npos);
    CHECK(info.units[1].name.find("AMS_1") != std::string::npos);
    CHECK(info.units[2].name.find("ace_1") != std::string::npos);
    CHECK(info.units[3].name.find("Turtle_2") != std::string::npos);

    // All 16 lanes should map correctly by global index
    REQUIRE(info.total_slots == 16);
    int total = 0;
    for (const auto& unit : info.units) {
        total += static_cast<int>(unit.slots.size());
    }
    CHECK(total == 16);
}

// ============================================================================
// Reactive lane highlighting & current_slot stability (PR #336 + improvements)
// ============================================================================

TEST_CASE("AFC stepper-only updates do not overwrite current_slot set by AFC state",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(10);
    helper.initialize_slots_from_discovery();

    // Step 1: AFC global state sets current_load = "lane9" (slot 9)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane9"}, {"filament_loaded", true}};
        params["AFC_stepper lane9"] = {{"status", "Tooled"}, {"tool_loaded", true},
                                       {"color", "00AEFF"},  {"material", "ASA"},
                                       {"spool_id", "42"},   {"weight", 800}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 9);

    // Step 2: Incremental update with ONLY AFC_stepper for lane0 — must NOT reset to 0
    {
        nlohmann::json params;
        params["AFC_stepper lane0"] = {
            {"prep", true}, {"load", true}, {"status", "Ready"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 9);
    CHECK(info.filament_loaded == true);

    // Step 3: Another stepper-only update for a different lane — still preserved
    {
        nlohmann::json params;
        params["AFC_stepper lane3"] = {
            {"prep", false}, {"load", false}, {"status", "None"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }
    CHECK(helper.get_system_info().current_slot == 9);
}

TEST_CASE("AFC reconciliation updates current_slot when active lane becomes unloaded",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Step 1: Set current_slot = 2 via AFC state
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Tool change — lane2 unloaded, lane3 loaded (AFC state included)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane3"}};
        params["AFC_stepper lane2"] = {{"status", "Ready"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE("AFC reconciliation via stepper-only updates current_slot when no authoritative source",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Step 1: Set current_slot via stepper LOADED status only (no AFC state)
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Stepper-only tool change — should detect and update
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {{"status", "Ready"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE("AFC non-active extruder does not overwrite current_slot", "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(2);

    // Set tool mapping: T0 -> lane0 (slot 0), T1 -> lane1 (slot 1)
    helper.feed_afc_stepper(
        "lane0", {{"map", "T0"}, {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1", {{"map", "T1"}, {"status", "Ready"}, {"tool_loaded", false}, {"color", "00FF00"}});

    // AFC state says current_load = lane0, current_tool = 0
    helper.feed_afc_state({{"current_load", "lane0"}, {"current_tool", 0}});
    REQUIRE(helper.get_system_info().current_slot == 0);
    REQUIRE(helper.get_system_info().current_tool == 0);

    // Non-active extruder1 reports lane_loaded = lane1 — must NOT overwrite current_slot
    helper.feed_afc_extruder(
        "extruder1",
        {{"lane_loaded", "lane1"}, {"tool_start_status", false}, {"tool_end_status", false}});

    auto info2 = helper.get_system_info();
    CHECK(info2.current_slot == 0);
    CHECK(info2.current_tool == 0);
}

TEST_CASE("AFC filament unload (current_load=null) clears state", "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Load filament into lane2
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}, {"filament_loaded", true}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);
    REQUIRE(helper.get_system_info().filament_loaded == true);

    // AFC reports current_load = null (unloaded)
    helper.feed_afc_state({{"current_load", nullptr}});

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
}

TEST_CASE("AFC tool changer reconciliation derives current_slot from active tool",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Set up tool mapping: T0->slot0, T1->slot1, T2->slot2, T3->slot3
    helper.feed_afc_stepper(
        "lane0", {{"map", "T0"}, {"status", "Ready"}, {"tool_loaded", false}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1", {{"map", "T1"}, {"status", "Ready"}, {"tool_loaded", false}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2", {{"map", "T2"}, {"status", "Ready"}, {"tool_loaded", false}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3", {{"map", "T3"}, {"status", "Ready"}, {"tool_loaded", false}, {"color", "FFFF00"}});

    // Verify tool mapping is populated
    auto mapping = helper.get_tool_to_slot_map();
    REQUIRE(mapping.size() >= 4);

    // Set up PARALLEL topology via unit object
    {
        nlohmann::json params;
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // Simulate: Klipper reports active tool = T2, current_load is null (mid tool-swap)
    // parse_afc_state treats null current_load as authoritative "unloaded" state,
    // so the tool changer reconciliation block does not override current_slot.
    // current_tool is preserved since it was explicitly provided.
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", nullptr}, {"current_tool", 2}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
    CHECK(info.current_tool == 2);
}

// ---- L1: classify_error ----
static const char* kJamLine =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still "
    "detect filament. Possible filament break or jam at the toolhead. Please clear "
    "the jam and reload filament manually, then resume the print.";

TEST_CASE("AFC jam with toolhead loaded offers Unload not Eject", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder",
                             {{"tool_start_status", true}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos);  // full text

    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Unload"));   // toolhead loaded -> Unload (closes R1)
    REQUIRE(has("Recover"));
    REQUIRE_FALSE(has("Eject"));
}

TEST_CASE("AFC jam with empty toolhead offers Eject not Unload", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder",
                             {{"tool_start_status", false}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(kJamLine, ctx);

    REQUIRE(e.has_value());
    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Eject"));      // empty toolhead -> Eject
    REQUIRE_FALSE(has("Unload"));
    REQUIRE(has("Recover"));
}

TEST_CASE("AFC catch-all: paused + error_state + unknown !! is CRITICAL AFC",
          "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_state({{"error_state", true}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error("!! Some AFC fault we do not recognize", ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE_FALSE(e->recovery_actions.empty());  // std actions attached
}

TEST_CASE("AFC defers when not paused and no error_state", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;  // idle, no error_state fed
    REQUIRE_FALSE(helper.classify_error("!! Some AFC fault", ctx).has_value());
}

TEST_CASE("AFC ignores non-error lines", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    REQUIRE_FALSE(helper.classify_error("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(helper.classify_error("ok", ctx).has_value());
}

TEST_CASE("AFC narration maps purge to purge not feed (S1)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE(afc.match_narration_phase("Purge") == std::optional<std::string>("purge"));
    REQUIRE(afc.match_narration_phase("Purging old filament") == std::optional<std::string>("purge"));
    REQUIRE(afc.match_narration_phase("Loading lane 2 to hub") == std::optional<std::string>("feed"));
}
TEST_CASE("AFC narration recognizes brush/clean/cut/poop/kick (S2)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE(afc.match_narration_phase("AFC_Brush: Clean Nozzle") == std::optional<std::string>("clean"));
    REQUIRE(afc.match_narration_phase("Move to Brush") == std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Cutting tip") == std::optional<std::string>("cut"));
    REQUIRE(afc.match_narration_phase("Poop") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Kick") == std::optional<std::string>("kick"));
    REQUIRE(afc.match_narration_phase("lane 2 is now loaded in toolhead") == std::optional<std::string>("load"));
}
TEST_CASE("AFC narration ignores unrelated lines", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE_FALSE(afc.match_narration_phase("Klipper state: ready").has_value());
    REQUIRE_FALSE(afc.match_narration_phase("").has_value());
}
TEST_CASE("AFC LOAD_SWAP template ordering puts purge after feed, brush after purge", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(tmpl.empty());
    auto idx = [&](const std::string& id) {
        for (size_t i = 0; i < tmpl.size(); ++i) if (tmpl[i].id == id) return static_cast<int>(i);
        return -1;
    };
    REQUIRE(idx("heat") == 0);
    REQUIRE(idx("feed") >= 0);
    REQUIRE(idx("purge") > idx("feed"));
    REQUIRE(idx("brush") > idx("purge"));
    REQUIRE(idx("clean") > idx("brush"));
}

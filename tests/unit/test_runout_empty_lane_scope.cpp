// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_empty_lane_scope.cpp
 * @brief Tests for backend-aware runout scoping (FilamentSensorManager::has_real_runout()).
 *
 * Run with: ./build/bin/helix-tests "[runout][scope]"
 *
 * Bug (Snapmaker U1, live): a multi-color print used heads 0 and 2; head 1 was
 * left INTENTIONALLY empty (never loaded). After the print completed, head 1's
 * empty lane raised a FALSE filament-runout alarm — its filament_motion_sensor
 * reads filament_detected=false, has_any_runout() returned true, and the idle
 * runout modal popped.
 *
 * has_real_runout() distinguishes "lane is EMPTY / never-loaded" (intentional,
 * not an alarm) from "lane was LOADED/present and lost filament mid-use" (real
 * runout). It maps each runout sensor (Snapmaker "e{N}_filament" -> slot N) to
 * its AMS lane and consults the backend's slot status.
 *
 * These tests drive a REAL AmsBackendSnapmaker (via filament_feed status) so
 * the lane EMPTY/AVAILABLE status is produced by production code, and a REAL
 * FilamentSensorManager fed via update_from_status().
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_subscription_backend.h"
#include "ams_types.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "ui_update_queue.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using json = nlohmann::json;

// Friend shim (declared as friend in ams_backend_snapmaker.h) to reach the
// protected handle_status_update() so tests can drive slot state through the
// real firmware parse. Distinct from test_ams_backend_snapmaker.cpp's
// SnapmakerTestAccess to avoid an ODR clash when both TUs link.
class RunoutScopeTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const json& n) {
        b.handle_status_update(n);
    }

    // Set a sensor's role directly, bypassing set_sensor_role()'s single-RUNOUT
    // exclusivity. Production reaches this multi-RUNOUT state via load_config
    // (settings.json restore), which writes sensor->role per entry with no
    // exclusivity check — exactly how a Snapmaker U1's four e{N}_filament
    // sensors all end up RUNOUT-roled.
    static void force_role(helix::FilamentSensorManager& mgr, const std::string& klipper,
                           helix::FilamentSensorRole role) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        for (auto& s : mgr.sensors_) {
            if (s.klipper_name == klipper) {
                s.role = role;
                s.enabled = true;
                return;
            }
        }
    }

    // Reset the manager to a clean slate (sensors + states cleared, grace
    // period already expired) so each test starts deterministic despite the
    // process-singleton.
    static void reset(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        mgr.sensors_.clear();
        mgr.states_.clear();
        mgr.master_enabled_ = true;
        mgr.sync_mode_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};

namespace {

// Builds a filament_feed status setting per-lane physical presence. detected[i]
// drives the Snapmaker backend's slot status: true -> AVAILABLE (present),
// false (and not LOADED) -> EMPTY. Lanes 0,1 go to "left", 2,3 to "right" to
// mirror the firmware split — but the backend reads both keys, so any split
// works; we put all four under "left" for simplicity.
json make_feed_status(const std::array<bool, 4>& detected) {
    json left = json::object();
    for (int i = 0; i < 4; ++i) {
        std::string key = (i == 0) ? "extruder0" : fmt::format("extruder{}", i);
        left[key] = json{{"filament_detected", detected[i]},
                         {"channel_state", "idle"},
                         {"channel_error", "ok"}};
    }
    return json{{"filament_feed left", left}};
}

// Feed a per-lane motion sensor reading into FilamentSensorManager. The
// Snapmaker per-tool sensors are "filament_motion_sensor e{N}_filament".
json make_sensor_status(int lane, bool filament_detected) {
    std::string key = fmt::format("filament_motion_sensor e{}_filament", lane);
    return json{{key, json{{"filament_detected", filament_detected}, {"enabled", true}}}};
}

// RAII: install a real Snapmaker backend into AmsState and remove it on scope
// exit so the global singleton is clean for other tests.
struct ScopedSnapmakerBackend {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    AmsBackendSnapmaker* backend = nullptr;

    ScopedSnapmakerBackend() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto be = std::make_unique<AmsBackendSnapmaker>(api.get(), nullptr);
        backend = be.get();
        AmsState::instance().set_backend(std::move(be));
    }
    ~ScopedSnapmakerBackend() { AmsState::instance().set_backend(nullptr); }

    void set_lane_presence(const std::array<bool, 4>& detected) {
        RunoutScopeTestAccess::handle_status(*backend, make_feed_status(detected));
    }
};

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

// Sets up FilamentSensorManager with four per-lane runout sensors enabled, all
// initially showing filament present. Roles are forced directly (mirroring the
// settings.json restore path) so all four lanes are RUNOUT-roled — the real
// Snapmaker U1 configuration, which set_sensor_role()'s exclusivity can't model.
void setup_four_lane_sensors(FilamentSensorManager& fsm) {
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    std::vector<std::string> names;
    for (int i = 0; i < 4; ++i) {
        names.push_back(fmt::format("filament_motion_sensor e{}_filament", i));
    }
    fsm.discover_sensors(names);
    for (int i = 0; i < 4; ++i) {
        std::string klipper = fmt::format("filament_motion_sensor e{}_filament", i);
        RunoutScopeTestAccess::force_role(fsm, klipper, FilamentSensorRole::RUNOUT);
        // Establish availability + present baseline.
        fsm.update_from_status(make_sensor_status(i, true));
    }
}

} // namespace

TEST_CASE("has_real_runout: empty/never-loaded lane sensor=false is NOT a runout",
          "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lane 1 is empty/never-loaded; lanes 0,2 present.
    backend.set_lane_presence({true, false, true, false});
    REQUIRE(AmsState::instance().get_backend()->get_slot_info(1).is_present() == false);

    // Lane 1's runout sensor reads no filament — but the lane is EMPTY.
    fsm.update_from_status(make_sensor_status(1, false));
    drain();

    // has_any_runout() (the OLD behavior) DOES fire — proving the sensor reads false.
    CHECK(fsm.has_any_runout());
    // has_real_runout() (the FIX) must NOT fire for an empty lane.
    CHECK_FALSE(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: loaded/present lane that loses filament IS a runout",
          "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lane 0 is present (loaded for the print).
    backend.set_lane_presence({true, false, false, false});
    REQUIRE(AmsState::instance().get_backend()->get_slot_info(0).is_present());

    // Lane 0 runs out mid-use: present lane, sensor now false.
    fsm.update_from_status(make_sensor_status(0, false));
    drain();

    CHECK(fsm.has_any_runout());
    // A genuine runout on a present/loaded lane MUST still alarm.
    CHECK(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: mixed heads 0,2 loaded + head 1 empty", "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Exact bug shape: heads 0 and 2 used (present), head 1 left empty.
    backend.set_lane_presence({true, false, true, false});

    // Head 1 (empty) sensor reads false post-print -> NO false runout.
    fsm.update_from_status(make_sensor_status(1, false));
    drain();
    CHECK_FALSE(fsm.has_real_runout());

    // Now head 0 (a LOADED lane) loses filament -> real runout MUST fire.
    fsm.update_from_status(make_sensor_status(0, false));
    drain();
    CHECK(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: no AMS backend -> behaves like has_any_runout", "[runout][scope]") {
    HelixTestFixture fx;
    AmsState::instance().set_backend(nullptr);
    auto& fsm = FilamentSensorManager::instance();

    // A plain single-extruder runout sensor whose name does NOT encode a lane.
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    fsm.discover_sensors({"filament_switch_sensor fsensor"});
    fsm.set_sensor_role("filament_switch_sensor fsensor", FilamentSensorRole::RUNOUT);
    fsm.set_sensor_enabled("filament_switch_sensor fsensor", true);
    fsm.update_from_status(json{{"filament_switch_sensor fsensor", json{{"filament_detected", true}}}});
    drain();
    CHECK_FALSE(fsm.has_real_runout());

    // Sensor goes empty: with no lane mapping, this is a real runout.
    fsm.update_from_status(
        json{{"filament_switch_sensor fsensor", json{{"filament_detected", false}}}});
    drain();
    CHECK(fsm.has_real_runout());
}

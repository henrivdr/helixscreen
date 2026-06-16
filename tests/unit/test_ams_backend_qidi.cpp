// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_qidi.h"
#include "ams_error.h"
#include "ams_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

#include "hv/json.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

// Friend-class shim per L065 — exposes private parse helpers for unit tests.
// Mirrors the Ad5xIfsTestAccess pattern in test_ams_backend_ad5x_ifs.cpp.
class QidiBoxTestAccess {
  public:
    static void parse_vars(AmsBackendQidi& b, const json& v) {
        b.parse_save_variables(v);
    }
    static void handle_status(AmsBackendQidi& b, const json& n) {
        b.handle_status_update(n);
    }
    static int filament_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).filament_id;
    }
    static int color_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).color_id;
    }
    static int vendor_id(const AmsBackendQidi& b, int slot) {
        return b.slot_rfid_.at(static_cast<size_t>(slot)).vendor_id;
    }
    static void apply_query(AmsBackendQidi& b, const json& response) {
        b.apply_query_response(response);
    }
    static void apply_filas_list(AmsBackendQidi& b, const std::string& content) {
        b.apply_filas_list(content);
    }
    static std::optional<AmsBackendQidi::FilaProfile> get_profile(
        const AmsBackendQidi& b, int fila_id) {
        auto it = b.fila_profiles_.find(fila_id);
        if (it == b.fila_profiles_.end())
            return std::nullopt;
        return it->second;
    }
    static std::optional<uint32_t> get_color(const AmsBackendQidi& b, int color_id) {
        auto it = b.color_palette_.find(color_id);
        if (it == b.color_palette_.end())
            return std::nullopt;
        return it->second;
    }
    static std::optional<std::string> get_vendor(const AmsBackendQidi& b, int vendor_id) {
        auto it = b.vendor_names_.find(vendor_id);
        if (it == b.vendor_names_.end())
            return std::nullopt;
        return it->second;
    }
    static size_t color_count(const AmsBackendQidi& b) {
        return b.color_palette_.size();
    }
    static size_t vendor_count(const AmsBackendQidi& b) {
        return b.vendor_names_.size();
    }
    static int resolve_fila_id(const std::map<int, AmsBackendQidi::FilaProfile>& profiles,
                               const std::string& material, const std::string& name) {
        return AmsBackendQidi::resolve_fila_id(profiles, material, name);
    }
    static int resolve_color_id(const std::map<int, uint32_t>& palette, uint32_t rgb) {
        return AmsBackendQidi::resolve_color_id(palette, rgb);
    }
    static int resolve_vendor_id(const std::map<int, std::string>& vendors,
                                 const std::string& brand) {
        return AmsBackendQidi::resolve_vendor_id(vendors, brand);
    }
    static DryerInfo get_dryer(const AmsBackendQidi& b) {
        return b.get_dryer_info();
    }
    static void set_clock(AmsBackendQidi& b, std::function<std::time_t()> fn) {
        b.now_fn_ = std::move(fn);
    }
    static void apply_box_extras(AmsBackendQidi& b, const json& e) {
        b.apply_box_extras(e);
    }
    static void set_drying_timer_supported(AmsBackendQidi& b, bool v) {
        b.drying_timer_supported_ = v;
    }
    static void apply_config_settings(AmsBackendQidi& b, const json& s) {
        b.apply_config_settings(s);
    }
};

// Subclass that captures execute_gcode() invocations so write-path tests
// can assert the exact gcode emitted without needing a real Moonraker.
class RecordingQidiBackend : public AmsBackendQidi {
  public:
    RecordingQidiBackend() : AmsBackendQidi(nullptr, nullptr) {}
    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    std::vector<std::string> sent;
};

// Build a Moonraker-shaped status notification carrying save_variables.
static json make_save_variables_notification(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// =====================================================================
// Type identification — pin down what the stub already advertises so
// later refactors don't silently change it.
// =====================================================================

TEST_CASE("QIDI Box type identification", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::QIDI_BOX);
    REQUIRE(backend.get_topology() == PathTopology::HUB);
}

TEST_CASE("QIDI Box default system_info shape", "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    auto info = backend.get_system_info();

    REQUIRE(info.type == AmsType::QIDI_BOX);
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[0].topology == PathTopology::HUB);
    // Unit must report as disconnected until enable_box=1 arrives.
    REQUIRE_FALSE(info.units[0].connected);
}

// =====================================================================
// parse_save_variables: enable_box gate
// =====================================================================
// `box_extras.py` reads `save_variables.variables.enable_box` and treats
// 0 as "Box installed but disabled" / 1 as "Box active." Mirror that
// onto AmsUnit::connected so the UI can show the right state.

TEST_CASE("QIDI Box parse_save_variables: enable_box=1 connects the unit",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::parse_vars(backend, json{{"enable_box", 1}});

    REQUIRE(backend.get_system_info().units[0].connected);
}

// =====================================================================
// parse_save_variables: box_count resizes the system
// =====================================================================
// `box_detect.py` writes save_variables.variables.box_count whenever USB
// enumeration changes. Each physical box = 4 slots, chainable up to 4
// boxes / 16 slots. The backend must resize the unit's slot vector to
// match so the UI shows the right slot count.

TEST_CASE("QIDI Box parse_save_variables: box_count=2 expands to 8 slots",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE(backend.get_system_info().total_slots == 4);

    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units[0].slot_count == 8);
    REQUIRE(info.units[0].slots.size() == 8);

    // Newly-added slots should be sensibly initialized.
    for (size_t i = 0; i < info.units[0].slots.size(); ++i) {
        REQUIRE(info.units[0].slots[i].slot_index == static_cast<int>(i));
        REQUIRE(info.units[0].slots[i].global_index == static_cast<int>(i));
    }
}

// =====================================================================
// parse_save_variables: per-slot state from slot<N> values
// =====================================================================
// box_stepper.py writes save_variables.variables.slot<N> as the slot's
// state machine cursor. From box_stepper.py LED-state mapping:
//   0   = empty / no filament
//   1   = filament loaded in box, retracted (available)
//   2   = filament loaded all the way to extruder
//   3   = mid-transition (loading/unloading in progress)
//   -1  = slot load failed
//   -2  = extruder load failed
//   -3  = runout-during-print detected by motion sensor

TEST_CASE("QIDI Box per-slot positive states map to SlotStatus",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot0", 0}, // empty
                                               {"slot1", 1}, // available (parked in box)
                                               {"slot2", 2}, // loaded to extruder
                                               {"slot3", 3}, // mid-transition
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::EMPTY);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    // Mid-transition: show as AVAILABLE so UI doesn't flicker — the
    // foreground action belongs on system_info_.action, not slot status.
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("QIDI Box per-slot negative states map to BLOCKED",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    SECTION("-1 = slot load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -1}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
    SECTION("-2 = extruder load failed") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -2}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
    SECTION("-3 = runout-during-print") {
        QidiBoxTestAccess::parse_vars(backend, json{{"slot0", -3}});
        REQUIRE(backend.get_system_info().units[0].slots[0].status ==
                SlotStatus::BLOCKED);
    }
}

// =====================================================================
// parse_save_variables: value_t<N> tool->slot mapping
// =====================================================================
// box_extras.py stores tool mappings as save_variables.variables.value_t<N>
// with value "slot<M>". This means "tool N prints from slot M." Default
// (when value_t<N> is missing) is tool N = slot N, which the resize
// code already establishes.

TEST_CASE("QIDI Box parse_save_variables: value_t<N>=slot<M> maps tool N to slot M",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot2"},
                                               {"value_t1", "slot3"},
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[2].mapped_tool == 0);
    REQUIRE(info.units[0].slots[3].mapped_tool == 1);
}

// =====================================================================
// handle_status_update routes save_variables changes through to parse
// =====================================================================
// Moonraker delivers save_variables changes inside notify_status_update as
// `{"save_variables": {"variables": {...}}}`. The backend must extract the
// inner variables payload and feed it to parse_save_variables so live
// updates flow through.

TEST_CASE("QIDI Box handle_status_update applies save_variables changes",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    QidiBoxTestAccess::handle_status(
        backend, make_save_variables_notification(json{
                     {"enable_box", 1},
                     {"box_count", 2},
                 }));

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].connected);
    REQUIRE(info.total_slots == 8);
}

TEST_CASE("QIDI Box handle_status_update ignores unrelated keys",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Notification without save_variables shouldn't touch state.
    QidiBoxTestAccess::handle_status(
        backend, json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
    REQUIRE(backend.get_system_info().total_slots == 4);
}

// =====================================================================
// last_load_slot: which slot is currently in the extruder
// =====================================================================
// box_extras.py is the source of truth for "which slot is loaded right
// now." Per-slot `slot<N>=2` is the secondary signal (and may be stale
// after error recovery). When last_load_slot is set, that slot must be
// LOADED and no other slot should claim LOADED.

TEST_CASE("QIDI Box last_load_slot promotes a single slot to LOADED",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot0", 1}, // available
                                               {"slot1", 1}, // available
                                               {"slot2", 1}, // available
                                               {"slot3", 1}, // available
                                               {"last_load_slot", "slot2"},
                                           });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    REQUIRE(info.units[0].slots[2].status == SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[3].status == SlotStatus::AVAILABLE);
}

TEST_CASE("QIDI Box last_load_slot=slot-1 means nothing is in the extruder",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Seed slot2 as loaded, then explicitly clear via last_load_slot
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot2", 2}, // claims LOADED
                                               {"last_load_slot", "slot-1"},
                                           });

    REQUIRE(backend.get_system_info().units[0].slots[2].status ==
            SlotStatus::AVAILABLE);
}

// =====================================================================
// parse_save_variables: RFID per-slot indices
// =====================================================================
// box_extras.py writes save_variables.variables.filament_slot<N> (1-99,
// index into officiall_filas_list.cfg), color_slot<N> (1-24, index into
// the color palette), and vendor_slot<N> (always 1 in the wild so far).
// The backend captures the raw IDs into a private side-table; resolution
// to material/color happens in a follow-up cycle once the cfg resolver
// lands.

TEST_CASE("QIDI Box filament_slot<N> captures raw RFID material index",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 42},
                                               {"filament_slot1", 7},
                                           });

    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 42);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 1) == 7);
    // Unset slots default to 0 (= unknown).
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 2) == 0);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 3) == 0);
}

TEST_CASE("QIDI Box color_slot<N> captures raw RFID color index",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"color_slot0", 3},  // some palette index
                                               {"color_slot2", 24}, // max palette index
                                           });

    REQUIRE(QidiBoxTestAccess::color_id(backend, 0) == 3);
    REQUIRE(QidiBoxTestAccess::color_id(backend, 2) == 24);
    REQUIRE(QidiBoxTestAccess::color_id(backend, 1) == 0);
}

TEST_CASE("QIDI Box vendor_slot<N> captures raw RFID vendor index",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"vendor_slot0", 1},
                                               {"vendor_slot3", 1},
                                           });

    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 0) == 1);
    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 3) == 1);
    REQUIRE(QidiBoxTestAccess::vendor_id(backend, 1) == 0);
}

TEST_CASE("QIDI Box RFID side-table resizes with box_count",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"box_count", 2},
                                               {"filament_slot7", 99},
                                           });

    REQUIRE(QidiBoxTestAccess::filament_id(backend, 7) == 99);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 0);
}

// =====================================================================
// handle_status_update: heater_box drying state + aht20_f humidity
// =====================================================================
// The QIDI Box has per-box drying: heater_generic heater_box<N> provides
// temperature + target, aht20_f heater_box<N> provides humidity. We
// surface the maximum across all boxes onto AmsUnit::environment so the
// UI can show "drying" when any box is active.

TEST_CASE("QIDI Box heater_generic heater_box1 populates unit environment",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].environment.has_value());

    QidiBoxTestAccess::handle_status(
        backend, json{{"heater_generic heater_box1",
                       json{{"temperature", 45.5}, {"target", 50.0}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(45.5).epsilon(0.01));
}

TEST_CASE("QIDI Box aht20_f heater_box1 populates humidity",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::handle_status(
        backend, json{{"aht20_f heater_box1",
                       json{{"temperature", 23.0}, {"humidity", 38.7}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->has_humidity);
    REQUIRE(info.units[0].environment->humidity_pct == Catch::Approx(38.7).epsilon(0.01));
}

TEST_CASE("QIDI Box multiple heater_box readings expose the maximum",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Need at least 2 boxes worth of slots for this to make sense.
    QidiBoxTestAccess::parse_vars(backend, json{{"box_count", 2}});

    // Box 1: hot drying. Box 2: idle. Max wins.
    QidiBoxTestAccess::handle_status(
        backend, json{
                     {"heater_generic heater_box1", json{{"temperature", 50.0}}},
                     {"heater_generic heater_box2", json{{"temperature", 22.5}}},
                 });

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(50.0).epsilon(0.01));
}

// =====================================================================
// apply_query_response: bootstrap from printer.objects.query result
// =====================================================================
// on_started() issues a printer.objects.query to fetch the initial state
// of save_variables (and per-box heater objects when they exist). The
// response shape is `{result: {status: {save_variables: {...}, ...}}}`.
// apply_query_response unwraps the result.status envelope and feeds the
// inner object through handle_status_update, reusing every parser we
// already test.

TEST_CASE("QIDI Box apply_query_response unwraps result.status and parses",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE_FALSE(backend.get_system_info().units[0].connected);

    json response = json{
        {"result", json{
                       {"status", json{
                                      {"save_variables",
                                       json{{"variables",
                                             json{{"enable_box", 1},
                                                  {"box_count", 2}}}}},
                                  }},
                   }},
    };
    QidiBoxTestAccess::apply_query(backend, response);

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].connected);
    REQUIRE(info.total_slots == 8);
}

TEST_CASE("QIDI Box apply_query_response handles missing result gracefully",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Wrong-shape response — must not crash, must not mutate state.
    QidiBoxTestAccess::apply_query(backend, json{{"error", "timed out"}});

    REQUIRE_FALSE(backend.get_system_info().units[0].connected);
}

TEST_CASE("QIDI Box notifications without heater data leave environment alone",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    // Seed an environment reading.
    QidiBoxTestAccess::handle_status(
        backend, json{{"heater_generic heater_box1",
                       json{{"temperature", 40.0}}}});
    REQUIRE(backend.get_system_info().units[0].environment.has_value());

    // Unrelated notification should not clobber.
    QidiBoxTestAccess::handle_status(
        backend, json{{"toolhead", {{"position", json::array({0, 0, 0, 0})}}}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].environment.has_value());
    REQUIRE(info.units[0].environment->temperature_c == Catch::Approx(40.0).epsilon(0.01));
}

// =====================================================================
// Write-path: always enabled (commands verified vs QIDI firmware, #1030)
// =====================================================================

TEST_CASE("QIDI Box load_filament: emits T<tool>",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    // Default mapping is tool=slot, so loading slot 2 emits T2.
    auto err = backend.load_filament(2);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "T2");
}

TEST_CASE("QIDI Box load_filament: respects value_t<N> tool mapping",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    // Map slot 3 to tool 0 via save_variables.
    QidiBoxTestAccess::parse_vars(backend, json{{"value_t0", "slot3"}});

    auto err = backend.load_filament(3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "T0");
}

TEST_CASE("QIDI Box unload_filament: emits UNLOAD_T<tool>",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.unload_filament(1);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "UNLOAD_T1");
}

TEST_CASE("QIDI Box unload_filament with -1 unloads the active slot",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Seed slot 2 as LOADED so unload_filament(-1) targets it.
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot2"}});

    auto err = backend.unload_filament(-1);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "UNLOAD_T2");
}

TEST_CASE("QIDI Box unload_filament with -1 and nothing loaded errors",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.unload_filament(-1);

    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box change_tool emits T<tool> directly",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.change_tool(3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "T3");
}

TEST_CASE("QIDI Box set_tool_mapping emits SAVE_VARIABLE for value_t<N>",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    auto err = backend.set_tool_mapping(/*tool=*/1, /*slot_idx=*/3);

    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"slot3\"");
}

// =====================================================================
// Full-stack integration: on_started actually fires the bootstrap query
// =====================================================================
// Unit-style tests (above) cover what we DO with response data, but they
// pass nullptr for the Moonraker stack so they don't prove on_started()
// even dispatches the query. This test wires up the full MoonrakerClientMock
// stack and asserts the dispatch happened with the expected method.
//
// One integration test catches the wiring; the unit-style tests cover the
// dense behaviour. Both have a job.

TEST_CASE("QIDI Box on_started dispatches printer.objects.query (integration)",
          "[ams][qidi_box][integration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendQidi backend(&api, &client);
    REQUIRE(client.last_send_method().empty());

    auto err = backend.start();
    REQUIRE(err.success());

    // start() calls on_started() which must dispatch printer.objects.query.
    // last_send_method() is captured synchronously inside the mock so we can
    // assert without UpdateQueue draining.
    REQUIRE(client.last_send_method() == "printer.objects.query");
}

TEST_CASE("QIDI Box write-path rejects out-of-range slot/tool indices",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;

    SECTION("load_filament: negative slot") {
        REQUIRE_FALSE(backend.load_filament(-1).success());
    }
    SECTION("load_filament: slot >= slot_count") {
        REQUIRE_FALSE(backend.load_filament(99).success());
    }
    SECTION("unload_filament: explicit out-of-range slot") {
        REQUIRE_FALSE(backend.unload_filament(99).success());
    }
    SECTION("change_tool: negative tool") {
        REQUIRE_FALSE(backend.change_tool(-1).success());
    }
    SECTION("set_tool_mapping: out-of-range") {
        REQUIRE_FALSE(backend.set_tool_mapping(-1, 0).success());
        REQUIRE_FALSE(backend.set_tool_mapping(0, 99).success());
    }
    REQUIRE(backend.sent.empty());
}

// =====================================================================
// apply_filas_list: parse officiall_filas_list.cfg (ConfigParser INI)
// =====================================================================
// box_extras.py looks up the printer-local file at
//   /home/mks/printer_data/config/officiall_filas_list.cfg
// using ConfigParser. Sections are `[fila<N>]` (N = filament_slot<N>
// index, 1-99) and each section carries min_temp / max_temp (nozzle)
// plus box_min_temp / box_max_temp (drying chamber). We fetch the file
// via Moonraker's file API and parse the same INI shape.

TEST_CASE("QIDI Box apply_filas_list parses sections into fila_profiles_",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
[fila1]
min_temp = 200
max_temp = 220
box_min_temp = 40
box_max_temp = 60

[fila2]
min_temp = 240
max_temp = 260
box_min_temp = 65
box_max_temp = 80
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);

    auto p1 = QidiBoxTestAccess::get_profile(backend, 1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->nozzle_min == 200);
    REQUIRE(p1->nozzle_max == 220);
    REQUIRE(p1->box_min == 40);
    REQUIRE(p1->box_max == 60);

    auto p2 = QidiBoxTestAccess::get_profile(backend, 2);
    REQUIRE(p2.has_value());
    REQUIRE(p2->nozzle_min == 240);
    REQUIRE(p2->nozzle_max == 260);
}

TEST_CASE("QIDI Box apply_filas_list tolerates whitespace, comments, blank lines",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
# leading comment
; semicolon-style comment

  [fila5]
    min_temp   =   195
   max_temp=215   ; inline tail (ignored)
box_min_temp = 35
box_max_temp = 55

# trailing comment
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);
    auto p = QidiBoxTestAccess::get_profile(backend, 5);
    REQUIRE(p.has_value());
    REQUIRE(p->nozzle_min == 195);
    REQUIRE(p->nozzle_max == 215);
    REQUIRE(p->box_min == 35);
    REQUIRE(p->box_max == 55);
}

TEST_CASE("QIDI Box apply_filas_list ignores non-fila sections",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    const std::string cfg = R"INI(
[printer]
kinematics = corexy

[fila7]
min_temp = 230
max_temp = 250
box_min_temp = 0
box_max_temp = 0
)INI";

    QidiBoxTestAccess::apply_filas_list(backend, cfg);
    REQUIRE(QidiBoxTestAccess::get_profile(backend, 7).has_value());
    // No spurious profile from [printer] (parses as fila_id 0 only if we
    // mistakenly accept any section).
    REQUIRE_FALSE(QidiBoxTestAccess::get_profile(backend, 0).has_value());
}

// =====================================================================
// last_load_slot also drives current_slot / current_tool / filament_loaded
// =====================================================================
// The AmsSubscriptionBackend base exposes these via get_current_slot() /
// get_current_tool() / is_filament_loaded(), reading directly from
// system_info_. Mirroring last_load_slot onto slot.status alone left the
// rest of the system at -1 / false even when something was clearly loaded.

TEST_CASE("QIDI Box last_load_slot populates current_slot/current_tool",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"slot2", 2},
                                               {"last_load_slot", "slot2"},
                                           });

    REQUIRE(backend.get_current_slot() == 2);
    // Default tool=slot mapping, so current_tool == current_slot.
    REQUIRE(backend.get_current_tool() == 2);
    REQUIRE(backend.is_filament_loaded());
}

TEST_CASE("QIDI Box last_load_slot=slot-1 clears current_*",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Seed loaded first.
    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot1"}});
    REQUIRE(backend.is_filament_loaded());

    QidiBoxTestAccess::parse_vars(backend, json{{"last_load_slot", "slot-1"}});

    REQUIRE_FALSE(backend.is_filament_loaded());
    REQUIRE(backend.get_current_slot() == -1);
    REQUIRE(backend.get_current_tool() == -1);
}

TEST_CASE("QIDI Box current_tool follows value_t mapping",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Map slot 3 to tool 0, then load it.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"value_t0", "slot3"},
                                               {"last_load_slot", "slot3"},
                                           });

    REQUIRE(backend.get_current_slot() == 3);
    REQUIRE(backend.get_current_tool() == 0);
}

// =====================================================================
// is_tool_change reflects through to AmsAction
// =====================================================================
// box_extras.py sets save_variables.is_tool_change=1 while
// _BOX_CHANGE_FILAMENT is running, clears it on completion. Map this
// onto AmsAction so the UI shows an in-flight indicator.

TEST_CASE("QIDI Box is_tool_change=1 sets action to LOADING",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);

    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 1}});

    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);
}

TEST_CASE("QIDI Box get_slot_info returns valid SlotInfo for expanded slots (box_count>1)",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Default backend is 4 slots; expand to 8 (box_count=2).
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"box_count", 2},
                                               {"slot5", 2}, // mark slot 5 LOADED
                                           });

    auto info = backend.get_slot_info(5);
    REQUIRE(info.slot_index == 5);
    REQUIRE(info.status == SlotStatus::LOADED);
    // Index past the expanded count still rejects.
    REQUIRE(backend.get_slot_info(99).slot_index == -1);
}

TEST_CASE("QIDI Box is_tool_change=0 returns action to IDLE",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 1}});
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    QidiBoxTestAccess::parse_vars(backend, json{{"is_tool_change", 0}});

    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("QIDI Box parse_save_variables applies cached profile to SlotInfo temps",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // Cache one profile, then mirror a slot pointing to it.
    QidiBoxTestAccess::apply_filas_list(backend,
                                        "[fila3]\nmin_temp=205\nmax_temp=225\n"
                                        "box_min_temp=45\nbox_max_temp=65\n");
    QidiBoxTestAccess::parse_vars(backend, json{{"filament_slot0", 3}});

    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 205);
    REQUIRE(info.units[0].slots[0].nozzle_temp_max == 225);
}

// =====================================================================
// Dryer capabilities (issue #1019)
// =====================================================================
// The QIDI Box has a PTC box heater that acts as a filament dryer.
// The backend must advertise dryer support with sane defaults so the
// UI shows the dryer control panel.

TEST_CASE("QIDI Box advertises dryer support with sane capability defaults",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    DryerInfo d = backend.get_dryer_info();
    REQUIRE(d.supported);
    REQUIRE(d.min_temp_c == Catch::Approx(35.0f));
    REQUIRE(d.max_temp_c == Catch::Approx(90.0f));   // settable ceiling, pre-config-query
    REQUIRE(d.max_duration_min == 720);
}

TEST_CASE("QIDI Box heater status populates dryer current/target temp",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::handle_status(
        backend, json{{"heater_generic heater_box1",
                       json{{"temperature", 48.0}, {"target", 55.0}}}});

    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.current_temp_c == Catch::Approx(48.0f).epsilon(0.01));
    REQUIRE(d.target_temp_c == Catch::Approx(55.0f).epsilon(0.01));
}

TEST_CASE("QIDI Box drying_state end_time drives remaining minutes",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1000}; });
    QidiBoxTestAccess::apply_box_extras(
        backend, json{{"box_drying_state",
                       json{{"box1", json{{"dry_state", 1}, {"end_time", 2800}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.active);
    REQUIRE(d.remaining_min == 30);
}

TEST_CASE("QIDI Box drying_state past end_time means not drying",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{5000}; });
    QidiBoxTestAccess::apply_box_extras(
        backend, json{{"box_drying_state",
                       json{{"box1", json{{"dry_state", 0}, {"end_time", 2800}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE_FALSE(d.active);
    REQUIRE(d.remaining_min == 0);
}

TEST_CASE("QIDI Box derives duration for externally-started drying (progress ring)",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::set_clock(backend, [] { return std::time_t{1000}; });
    // 60 min remaining, started outside HelixScreen (no commanded duration).
    QidiBoxTestAccess::apply_box_extras(
        backend, json{{"box_drying_state",
                       json{{"box1", json{{"dry_state", 1}, {"end_time", 4600}}}}}});
    DryerInfo d = QidiBoxTestAccess::get_dryer(backend);
    REQUIRE(d.duration_min == 60);
    REQUIRE(d.get_progress_pct() == 0); // just started: 60/60 remaining
}

TEST_CASE("QIDI Box config query refines max temp (heater_generic section)",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_config_settings(
        backend, json{{"heater_generic heater_box1", json{{"max_temp", 80.0}}}});
    REQUIRE(QidiBoxTestAccess::get_dryer(backend).max_temp_c == Catch::Approx(80.0f));
}

TEST_CASE("QIDI Box config query refines max temp (box_config section)",
          "[ams][qidi_box][dryer]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_config_settings(
        backend,
        json{{"box_config box0", json{{"target_max_temp_heater_generic", 90.0}}}});
    REQUIRE(QidiBoxTestAccess::get_dryer(backend).max_temp_c == Catch::Approx(90.0f));
}

// =====================================================================
// Dryer write-path: start_drying / stop_drying (issue #1019)
// =====================================================================

TEST_CASE("QIDI Box start_drying uses ENABLE_BOX_DRY when timer supported",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);
    auto err = backend.start_drying(55.0f, 240);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "ENABLE_BOX_DRY BOX=1 TEMP=55 END_TIME=4");
}

TEST_CASE("QIDI Box start_drying falls back to SET_HEATER_TEMPERATURE",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, false);
    auto err = backend.start_drying(55.0f, 240);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=55");
}

TEST_CASE("QIDI Box start_drying rejects out-of-range temp",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    auto err = backend.start_drying(150.0f, 240);
    REQUIRE_FALSE(err.success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box stop_drying uses DISABLE_BOX_DRY when timer supported",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, true);
    auto err = backend.stop_drying(0);
    REQUIRE(err.success());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "DISABLE_BOX_DRY BOX=1");
}

TEST_CASE("QIDI Box stop_drying falls back to TARGET=0",
          "[ams][qidi_box][dryer][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::set_drying_timer_supported(backend, false);
    auto err = backend.stop_drying(0);
    REQUIRE(err.success());
    REQUIRE(backend.sent[0] == "SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=0");
}

// =====================================================================
// apply_filas_list: extended parse — filament name + type, colordict,
// vendor_list (stock QIDI officiall_filas_list.cfg, real excerpts)
// =====================================================================

// A trimmed-but-real excerpt of the stock file: a few fila sections plus
// the colordict and vendor_list tail. Mirrors the real ConfigParser
// alignment (key/value separated by a run of spaces around `=`).
static const char* kStockFilasExcerpt = R"INI(
[fila1]
filament                       = PLA Rapido
min_temp                       = 190
max_temp                       = 240
box_min_temp                   = 0
box_max_temp                   = 0
type                           = PLA

[fila11]
filament                       = ABS Rapido
min_temp                       = 240
max_temp                       = 280
box_min_temp                   = 0
box_max_temp                   = 45
type                           = ABS

[fila42]
filament                       = PETG-CF
min_temp                       = 240
max_temp                       = 270
box_min_temp                   = 0
box_max_temp                   = 45
type                           = PETG-CF

[colordict]
1                              = #FAFAFA
2                              = #060606
18                             = #FF362D
24                             = #B87F2B

[vendor_list]
0                              = Generic
1                              = QIDI
2                              = eSUN
)INI";

TEST_CASE("QIDI Box apply_filas_list captures filament name and type",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    auto p1 = QidiBoxTestAccess::get_profile(backend, 1);
    REQUIRE(p1.has_value());
    REQUIRE(p1->name == "PLA Rapido");
    REQUIRE(p1->type == "PLA");
    REQUIRE(p1->nozzle_min == 190);
    REQUIRE(p1->nozzle_max == 240);

    auto p11 = QidiBoxTestAccess::get_profile(backend, 11);
    REQUIRE(p11.has_value());
    REQUIRE(p11->name == "ABS Rapido");
    REQUIRE(p11->type == "ABS");
    REQUIRE(p11->box_max == 45);

    auto p42 = QidiBoxTestAccess::get_profile(backend, 42);
    REQUIRE(p42.has_value());
    REQUIRE(p42->name == "PETG-CF");
    REQUIRE(p42->type == "PETG-CF");
}

TEST_CASE("QIDI Box apply_filas_list parses colordict to packed RGB",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    REQUIRE(QidiBoxTestAccess::get_color(backend, 1) == 0xFAFAFA);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 2) == 0x060606);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 18) == 0xFF362D);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 24) == 0xB87F2B);
    REQUIRE_FALSE(QidiBoxTestAccess::get_color(backend, 99).has_value());
}

TEST_CASE("QIDI Box apply_filas_list parses vendor_list",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 0) == "Generic");
    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 1) == "QIDI");
    REQUIRE(QidiBoxTestAccess::get_vendor(backend, 2) == "eSUN");
    REQUIRE_FALSE(QidiBoxTestAccess::get_vendor(backend, 99).has_value());
}

TEST_CASE("QIDI Box apply_filas_list colordict accepts bare hex (no #)",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend,
                                        "[colordict]\n1 = FAFAFA\n2 = #060606\n");
    REQUIRE(QidiBoxTestAccess::get_color(backend, 1) == 0xFAFAFA);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 2) == 0x060606);
}

TEST_CASE("QIDI Box apply_filas_list ignores bad sections and atomically swaps",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // First load populates all three maps.
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    REQUIRE(QidiBoxTestAccess::get_color(backend, 1).has_value());

    // Reload with only fila data + a typo'd section — colordict/vendor should
    // be cleared (atomic replace), not merged.
    QidiBoxTestAccess::apply_filas_list(backend, R"INI(
[colourdict]
1 = #112233

[fila3]
filament = PLA Metal
type = PLA
min_temp = 190
max_temp = 240
)INI");
    REQUIRE(QidiBoxTestAccess::get_profile(backend, 3).has_value());
    // Old fila1 gone (replaced).
    REQUIRE_FALSE(QidiBoxTestAccess::get_profile(backend, 1).has_value());
    // Typo'd [colourdict] not accepted, old palette wiped.
    REQUIRE(QidiBoxTestAccess::color_count(backend) == 0);
    REQUIRE(QidiBoxTestAccess::vendor_count(backend) == 0);
}

TEST_CASE("QIDI Box apply_filas_list still parses temps (regression)",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);
    auto p11 = QidiBoxTestAccess::get_profile(backend, 11);
    REQUIRE(p11.has_value());
    REQUIRE(p11->nozzle_min == 240);
    REQUIRE(p11->nozzle_max == 280);
    REQUIRE(p11->box_min == 0);
    REQUIRE(p11->box_max == 45);
}

// =====================================================================
// Read-path resolution: slot_rfid_ ids → SlotInfo material/color/brand
// =====================================================================
// Once the filas list is cached, parse_save_variables must resolve the raw
// filament_slot/color_slot/vendor_slot indices onto the SlotInfo fields the
// UI reads (material, color_rgb, brand) — in addition to the nozzle temps it
// already applied.

TEST_CASE("QIDI Box read-path resolves material/color/brand from filas list",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 11}, // ABS Rapido / ABS
                                               {"color_slot0", 18},    // #FF362D
                                               {"vendor_slot0", 2},    // eSUN
                                           });

    auto info = backend.get_system_info();
    const auto& s = info.units[0].slots[0];
    REQUIRE(s.material == "ABS");
    REQUIRE(s.color_rgb == 0xFF362D);
    REQUIRE(s.brand == "eSUN");
    // Temps still applied from the same profile.
    REQUIRE(s.nozzle_temp_min == 240);
    REQUIRE(s.nozzle_temp_max == 280);
}

TEST_CASE("QIDI Box read-path leaves fields unchanged when ids miss",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    // Unknown filament id (not in the cfg) + unknown color/vendor ids.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 77},
                                               {"color_slot0", 99},
                                               {"vendor_slot0", 88},
                                           });

    auto info = backend.get_system_info();
    const auto& s = info.units[0].slots[0];
    // Material/brand untouched (empty defaults), color stays at default.
    REQUIRE(s.material.empty());
    REQUIRE(s.brand.empty());
    REQUIRE(s.color_rgb == AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE("QIDI Box read-path resolution survives before filas list loads",
          "[ams][qidi_box]") {
    AmsBackendQidi backend(nullptr, nullptr);
    // No filas list yet — ids captured but nothing resolved, no crash.
    QidiBoxTestAccess::parse_vars(backend, json{
                                               {"filament_slot0", 1},
                                               {"color_slot0", 1},
                                               {"vendor_slot0", 1},
                                           });
    auto info = backend.get_system_info();
    REQUIRE(info.units[0].slots[0].material.empty());
    REQUIRE(info.units[0].slots[0].color_rgb == AMS_DEFAULT_SLOT_COLOR);
    REQUIRE(QidiBoxTestAccess::filament_id(backend, 0) == 1);
}

// =====================================================================
// Reverse lookups (pure) for set_slot_info()
// =====================================================================

TEST_CASE("QIDI Box resolve_fila_id matches name then falls back to type",
          "[ams][qidi_box]") {
    std::map<int, AmsBackendQidi::FilaProfile> profiles;
    profiles[1] = {"PLA Rapido", "PLA", 190, 240, 0, 0};
    profiles[11] = {"ABS Rapido", "ABS", 240, 280, 0, 45};
    profiles[42] = {"PETG-CF", "PETG-CF", 240, 270, 0, 45};

    // Exact (case-insensitive) name match wins.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "ABS", "abs rapido") == 11);
    // No name match → first profile whose type matches material.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "pla", "") == 1);
    // Nothing matches → 0.
    REQUIRE(QidiBoxTestAccess::resolve_fila_id(profiles, "NYLON", "Whatever") == 0);
}

TEST_CASE("QIDI Box resolve_color_id picks nearest palette entry",
          "[ams][qidi_box]") {
    std::map<int, uint32_t> palette;
    palette[1] = 0xFAFAFA; // near-white
    palette[2] = 0x060606; // near-black
    palette[18] = 0xFF362D; // red

    // Pure white → near-white entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0xFFFFFF) == 1);
    // Pure black → near-black entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0x000000) == 2);
    // Reddish → red entry.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(palette, 0xEE2020) == 18);
    // Empty palette → 0.
    REQUIRE(QidiBoxTestAccess::resolve_color_id(std::map<int, uint32_t>{}, 0x123456) == 0);
}

TEST_CASE("QIDI Box resolve_vendor_id matches name, falls back to Generic",
          "[ams][qidi_box]") {
    std::map<int, std::string> vendors;
    vendors[0] = "Generic";
    vendors[1] = "QIDI";
    vendors[2] = "eSUN";

    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "esun") == 2);
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "QIDI") == 1);
    // Unknown brand → Generic id.
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(vendors, "Polymaker") == 0);
    // No Generic present and no match → 0.
    std::map<int, std::string> no_generic{{5, "QIDI"}};
    REQUIRE(QidiBoxTestAccess::resolve_vendor_id(no_generic, "Polymaker") == 0);
}

// =====================================================================
// set_slot_info: write reverse-mapped ids back to save_variables
// =====================================================================

TEST_CASE("QIDI Box set_slot_info emits SAVE_VARIABLE for all three ids",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SlotInfo info;
    info.material = "ABS";
    info.brand = "eSUN";
    info.color_rgb = 0xFF362D;

    auto err = backend.set_slot_info(0, info, /*persist=*/true);
    REQUIRE(err.success());

    // Three writes: filament_slot0 / color_slot0 / vendor_slot0 — integers
    // unquoted (matches Klipper SAVE_VARIABLE for numeric values).
    std::vector<std::string> sent = backend.sent;
    REQUIRE(sent.size() == 3);
    bool saw_fila = false, saw_color = false, saw_vendor = false;
    for (const auto& g : sent) {
        if (g == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=11")
            saw_fila = true;
        if (g == "SAVE_VARIABLE VARIABLE=color_slot0 VALUE=18")
            saw_color = true;
        if (g == "SAVE_VARIABLE VARIABLE=vendor_slot0 VALUE=2")
            saw_vendor = true;
    }
    REQUIRE(saw_fila);
    REQUIRE(saw_color);
    REQUIRE(saw_vendor);
}

TEST_CASE("QIDI Box set_slot_info skips fields with no mapping",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    // Material that doesn't map to any fila; valid color + vendor.
    SlotInfo info;
    info.material = "NYLON-X";
    info.brand = "QIDI";
    info.color_rgb = 0xFAFAFA;

    auto err = backend.set_slot_info(1, info, /*persist=*/true);
    REQUIRE(err.success());
    // No filament_slot write (unmapped), but color + vendor present.
    for (const auto& g : backend.sent) {
        REQUIRE(g.find("filament_slot") == std::string::npos);
    }
    bool saw_color = false, saw_vendor = false;
    for (const auto& g : backend.sent) {
        if (g == "SAVE_VARIABLE VARIABLE=color_slot1 VALUE=1")
            saw_color = true;
        if (g == "SAVE_VARIABLE VARIABLE=vendor_slot1 VALUE=1")
            saw_vendor = true;
    }
    REQUIRE(saw_color);
    REQUIRE(saw_vendor);
}

TEST_CASE("QIDI Box set_slot_info rejects out-of-range slot index",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    QidiBoxTestAccess::apply_filas_list(backend, kStockFilasExcerpt);

    SlotInfo info;
    info.material = "PLA";
    REQUIRE_FALSE(backend.set_slot_info(-1, info, true).success());
    REQUIRE_FALSE(backend.set_slot_info(99, info, true).success());
    REQUIRE(backend.sent.empty());
}

TEST_CASE("QIDI Box set_slot_info with no palette/vendor data still writes fila",
          "[ams][qidi_box][write_path]") {
    RecordingQidiBackend backend;
    // Only fila profiles loaded — no colordict / vendor_list.
    QidiBoxTestAccess::apply_filas_list(backend,
                                        "[fila1]\nfilament = PLA Rapido\ntype = PLA\n"
                                        "min_temp = 190\nmax_temp = 240\n");
    SlotInfo info;
    info.material = "PLA";
    info.brand = "eSUN";
    info.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, info, true);
    REQUIRE(err.success());
    // Only the filament_slot write — empty palette/vendor skip cleanly.
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(backend.sent[0] == "SAVE_VARIABLE VARIABLE=filament_slot0 VALUE=1");
}

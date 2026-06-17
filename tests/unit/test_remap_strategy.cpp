// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin each backend's RemapStrategy advertisement.
//
// RemapStrategy tells the preflight filament validator how a backend routes
// tool->material assignments through to the printer:
//   None         — no mapping (base / default); also used for ACE which uses
//                  ACE_CHANGE_TOOL TOOL=n rather than the Tn/SM_PRINT_* families
//                  that GcodeToolRemapper handles (remap unimplemented for ACE)
//   Native       — backend owns the T0..Tn slot mapping internally (HH, AFC,
//                  CFS, AD5X IFS, ToolChanger); helix does NOT rewrite gcode
//   GcodeRewrite — helix must rewrite T-commands in the gcode file because the
//                  backend has no internal tool-routing (Snapmaker U1)
//
// Backends that need nullptr-constructible probes follow the pattern established
// in test_ams_backend_afc_capabilities.cpp.

#include "ams_backend.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"

#include "../catch_amalgamated.hpp"

namespace {

// Minimal probes — constructed with nullptr api/client so no Moonraker
// connection is required.  These mirror the pattern in
// test_ams_backend_afc_capabilities.cpp.

class AfcProbe : public AmsBackendAfc {
  public:
    AfcProbe() : AmsBackendAfc(nullptr, nullptr) {}
};

class HappyHareProbe : public AmsBackendHappyHare {
  public:
    HappyHareProbe() : AmsBackendHappyHare(nullptr, nullptr) {}
};

class CfsProbe : public helix::printer::AmsBackendCfs {
  public:
    CfsProbe() : helix::printer::AmsBackendCfs(nullptr, nullptr) {}
};

class Ad5xIfsProbe : public AmsBackendAd5xIfs {
  public:
    Ad5xIfsProbe() : AmsBackendAd5xIfs(nullptr, nullptr) {}
};

class ToolChangerProbe : public AmsBackendToolChanger {
  public:
    ToolChangerProbe() : AmsBackendToolChanger(nullptr, nullptr) {}
};

class SnapmakerProbe : public AmsBackendSnapmaker {
  public:
    SnapmakerProbe() : AmsBackendSnapmaker(nullptr, nullptr) {}
};

class AceProbe : public AmsBackendAce {
  public:
    AceProbe() : AmsBackendAce(nullptr, nullptr) {}
};

// Minimal concrete subclass of the base to test the default.
class BaseProbe : public AmsBackend {
  public:
    // Stubs for pure-virtual methods — only strategy is under test.
    AmsError start() override { return AmsErrorHelper::success(); }
    void stop() override {}
    [[nodiscard]] bool is_running() const override { return false; }
    void set_event_callback(EventCallback) override {}
    [[nodiscard]] AmsSystemInfo get_system_info() const override { return {}; }
    [[nodiscard]] AmsType get_type() const override { return AmsType::NONE; }
    [[nodiscard]] SlotInfo get_slot_info(int) const override { return {}; }
    [[nodiscard]] AmsAction get_current_action() const override { return AmsAction::IDLE; }
    [[nodiscard]] int get_current_tool() const override { return -1; }
    [[nodiscard]] int get_current_slot() const override { return -1; }
    [[nodiscard]] bool is_filament_loaded() const override { return false; }
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::LINEAR; }
    [[nodiscard]] PathSegment get_filament_segment() const override {
        return PathSegment::NONE;
    }
    [[nodiscard]] PathSegment get_slot_filament_segment(int) const override {
        return PathSegment::NONE;
    }
    [[nodiscard]] PathSegment infer_error_segment() const override { return PathSegment::NONE; }
    AmsError load_filament(int) override { return AmsErrorHelper::success(); }
    AmsError unload_filament(int) override { return AmsErrorHelper::success(); }
    AmsError select_slot(int) override { return AmsErrorHelper::success(); }
    AmsError change_tool(int) override { return AmsErrorHelper::success(); }
    AmsError recover() override { return AmsErrorHelper::success(); }
    AmsError reset() override { return AmsErrorHelper::success(); }
    AmsError cancel() override { return AmsErrorHelper::success(); }
    AmsError set_slot_info(int, const SlotInfo&, bool) override { return AmsErrorHelper::success(); }
    AmsError set_tool_mapping(int, int) override { return AmsErrorHelper::success(); }
    AmsError enable_bypass() override { return AmsErrorHelper::success(); }
    AmsError disable_bypass() override { return AmsErrorHelper::success(); }
    [[nodiscard]] bool is_bypass_active() const override { return false; }
};

} // namespace

TEST_CASE("Native-strategy backends return RemapStrategy::Native", "[ams][strategy]") {
    SECTION("AFC") {
        AfcProbe afc;
        REQUIRE(afc.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("Happy Hare") {
        HappyHareProbe hh;
        REQUIRE(hh.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("CFS") {
        CfsProbe cfs;
        REQUIRE(cfs.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("AD5X IFS") {
        Ad5xIfsProbe ad5x;
        REQUIRE(ad5x.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
    SECTION("ToolChanger") {
        ToolChangerProbe tc;
        REQUIRE(tc.get_remap_strategy() == AmsBackend::RemapStrategy::Native);
    }
}

TEST_CASE("Snapmaker returns RemapStrategy::SnapmakerNative", "[ams][strategy]") {
    SECTION("Snapmaker") {
        SnapmakerProbe sm;
        REQUIRE(sm.get_remap_strategy() == AmsBackend::RemapStrategy::SnapmakerNative);
    }
}

TEST_CASE("Base AmsBackend default returns RemapStrategy::None", "[ams][strategy]") {
    BaseProbe base;
    REQUIRE(base.get_remap_strategy() == AmsBackend::RemapStrategy::None);
}

TEST_CASE("ACE returns RemapStrategy::None (GcodeRewrite unimplemented)", "[ams][strategy]") {
    // ACE uses ACE_CHANGE_TOOL TOOL=n, not the Tn/SM_PRINT_* families that
    // GcodeToolRemapper handles, so remap is disabled until that command family
    // is implemented and validated on a real ACE file.
    AceProbe ace;
    REQUIRE(ace.get_remap_strategy() == AmsBackend::RemapStrategy::None);
}

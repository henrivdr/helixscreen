// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "config.h"
#include "filament_database.h"
#include "material_settings_manager.h"

#include "../catch_amalgamated.hpp"

namespace helix {
class TestAccess {
  public:
    static void reset(MaterialSettingsManager& mgr) {
        mgr.overrides_.clear();
        mgr.initialized_ = false;
    }
};
} // namespace helix

using namespace helix;
using namespace filament;

// Fixture that resets MaterialSettingsManager singleton between tests
struct MaterialSettingsFixture : LVGLTestFixture {
    ~MaterialSettingsFixture() override {
        TestAccess::reset(MaterialSettingsManager::instance());
    }
};

// ============================================================================
// MaterialSettingsManager Tests
// ============================================================================

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager init with no config",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // No overrides should exist with fresh config
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("PLA"));
    CHECK(MaterialSettingsManager::instance().get_override("PLA") == nullptr);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager set/get round trip",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 195;
    ovr.nozzle_max = 215;
    ovr.bed_temp = 55;

    MaterialSettingsManager::instance().set_override("PLA", ovr);

    REQUIRE(MaterialSettingsManager::instance().has_override("PLA"));
    const auto* result = MaterialSettingsManager::instance().get_override("PLA");
    REQUIRE(result != nullptr);
    REQUIRE(result->nozzle_min.has_value());
    CHECK(*result->nozzle_min == 195);
    REQUIRE(result->nozzle_max.has_value());
    CHECK(*result->nozzle_max == 215);
    REQUIRE(result->bed_temp.has_value());
    CHECK(*result->bed_temp == 55);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager sparse override",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Only override bed temp
    MaterialOverride ovr;
    ovr.bed_temp = 110;

    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    CHECK_FALSE(result->nozzle_min.has_value());
    CHECK_FALSE(result->nozzle_max.has_value());
    REQUIRE(result->bed_temp.has_value());
    CHECK(*result->bed_temp == 110);

    // Clean up
    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager clear_override",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 200;
    MaterialSettingsManager::instance().set_override("PETG", ovr);
    REQUIRE(MaterialSettingsManager::instance().has_override("PETG"));

    MaterialSettingsManager::instance().clear_override("PETG");
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("PETG"));
    CHECK(MaterialSettingsManager::instance().get_override("PETG") == nullptr);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager clear nonexistent is safe",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Should not crash
    MaterialSettingsManager::instance().clear_override("NonExistent");
    CHECK_FALSE(MaterialSettingsManager::instance().has_override("NonExistent"));
}

// ============================================================================
// find_material override integration tests
// ============================================================================

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns overridden nozzle temps",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Set override for PLA
    MaterialOverride ovr;
    ovr.nozzle_min = 195;
    ovr.nozzle_max = 215;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 195);
    CHECK(result->nozzle_max == 215);
    CHECK(result->bed_temp == 60); // Not overridden, should be database default

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns overridden bed temp only",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 190); // Database default
    CHECK(result->nozzle_max == 220); // Database default
    CHECK(result->bed_temp == 55);    // Overridden

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material returns defaults after clear_override",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.nozzle_min = 200;
    ovr.bed_temp = 70;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    MaterialSettingsManager::instance().clear_override("PLA");

    auto result = find_material("PLA");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 190);
    CHECK(result->nozzle_max == 220);
    CHECK(result->bed_temp == 60);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material with no override returns database values",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Ensure no overrides for PETG
    MaterialSettingsManager::instance().clear_override("PETG");

    auto result = find_material("PETG");
    REQUIRE(result.has_value());
    CHECK(result->nozzle_min == 230);
    CHECK(result->nozzle_max == 260);
    CHECK(result->bed_temp == 80);
}

TEST_CASE_METHOD(MaterialSettingsFixture, "find_material override preserves non-temp fields",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    auto result = find_material("PLA");
    REQUIRE(result.has_value());

    // Non-temperature fields should be unchanged
    CHECK(std::string_view(result->name) == "PLA");
    CHECK(std::string_view(result->category) == "Standard");
    CHECK(result->dry_temp_c == 45);
    CHECK(result->density_g_cm3 == Catch::Approx(1.24f));
    CHECK(std::string_view(result->compat_group) == "PLA");

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "Multiple material overrides coexist",
                 "[material_settings][filament]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride pla_ovr;
    pla_ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", pla_ovr);

    MaterialOverride abs_ovr;
    abs_ovr.nozzle_min = 245;
    abs_ovr.bed_temp = 110;
    MaterialSettingsManager::instance().set_override("ABS", abs_ovr);

    auto pla = find_material("PLA");
    auto abs = find_material("ABS");
    REQUIRE(pla.has_value());
    REQUIRE(abs.has_value());

    CHECK(pla->bed_temp == 55);
    CHECK(abs->nozzle_min == 245);
    CHECK(abs->bed_temp == 110);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager preheat_macro round trip",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "PREHEAT_ABS";
    ovr.macro_handles_heating = true;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());
    CHECK(*result->preheat_macro == "PREHEAT_ABS");
    REQUIRE(result->macro_handles_heating.has_value());
    CHECK(*result->macro_handles_heating == true);

    CHECK_FALSE(result->nozzle_min.has_value());
    CHECK_FALSE(result->nozzle_max.has_value());
    CHECK_FALSE(result->bed_temp.has_value());

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "MaterialSettingsManager macro with temp overrides",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.bed_temp = 110;
    ovr.preheat_macro = "HEAT_SOAK_ABS";
    ovr.macro_handles_heating = false;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    CHECK(*result->bed_temp == 110);
    CHECK(*result->preheat_macro == "HEAT_SOAK_ABS");
    CHECK(*result->macro_handles_heating == false);

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "MaterialSettingsManager absent macro_handles_heating defaults true",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "MY_MACRO";
    MaterialSettingsManager::instance().set_override("TPU", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("TPU");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());
    CHECK_FALSE(result->macro_handles_heating.has_value());

    MaterialSettingsManager::instance().clear_override("TPU");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "Preheat macro override: macro_handles_heating true skips temps",
                 "[material_settings][preheat]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "PREHEAT_ABS";
    ovr.macro_handles_heating = true;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);
    REQUIRE(result->preheat_macro.has_value());

    bool handles_heating = result->macro_handles_heating.value_or(true);
    CHECK(handles_heating == true);
    CHECK(*result->preheat_macro == "PREHEAT_ABS");

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture,
                 "Preheat macro override: macro_handles_heating false runs both",
                 "[material_settings][preheat]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    MaterialOverride ovr;
    ovr.preheat_macro = "ENABLE_BED_FANS";
    ovr.macro_handles_heating = false;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto* result = MaterialSettingsManager::instance().get_override("ABS");
    REQUIRE(result != nullptr);

    bool handles_heating = result->macro_handles_heating.value_or(true);
    CHECK(handles_heating == false);

    MaterialSettingsManager::instance().clear_override("ABS");
}

TEST_CASE_METHOD(MaterialSettingsFixture, "get_all_overrides returns all set overrides",
                 "[material_settings]") {
    Config::get_instance();
    MaterialSettingsManager::instance().init();

    // Clear any stale overrides
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");

    MaterialOverride ovr;
    ovr.bed_temp = 55;
    MaterialSettingsManager::instance().set_override("PLA", ovr);

    ovr.bed_temp = 110;
    MaterialSettingsManager::instance().set_override("ABS", ovr);

    const auto& all = MaterialSettingsManager::instance().get_all_overrides();
    CHECK(all.count("PLA") == 1);
    CHECK(all.count("ABS") == 1);

    // Clean up
    MaterialSettingsManager::instance().clear_override("PLA");
    MaterialSettingsManager::instance().clear_override("ABS");
}

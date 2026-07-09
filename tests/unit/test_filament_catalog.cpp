// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "helix_test_fixture.h"
#include "../catch_amalgamated.hpp"

using helix::printer::FilamentCatalog;

namespace {
constexpr const char* FIX = "tests/fixtures/filaments_test.json";
}

TEST_CASE_METHOD(HelixTestFixture, "resolve_code cfs hit and miss", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, /*codes_only=*/true, "cfs");
    const auto* p = cat.resolve_code("cfs", "01001");
    REQUIRE(p != nullptr);
    CHECK(p->brand == "Creality");
    CHECK(p->type == "PLA");
    CHECK(cat.resolve_code("cfs", "99999") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "effective inherits type range when thin", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* pla = cat.resolve_id("polymaker-pla-pro");   // no explicit range
    REQUIRE(pla != nullptr);
    CHECK(pla->nozzle_min == 190);   // inherited from PLA type
    CHECK(pla->nozzle_max == 220);
    CHECK(pla->bed_temp == 60);      // inherited from PLA type
    CHECK(pla->compat_group == std::string("PLA"));
}

TEST_CASE_METHOD(HelixTestFixture, "explicit override wins over type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 270);   // explicit, outside generic ABS range
    CHECK(abs->nozzle_max == 290);
    CHECK(abs->bed_temp == 105);     // explicit bed
}

TEST_CASE_METHOD(HelixTestFixture, "load_codes materializes only coded slice", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, true, "cfs");
    CHECK(cat.all_products().size() == 1);          // only the cfs-coded entry
    CHECK(cat.resolve_id("polymaker-abs-pro") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "queries by brand and type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    CHECK(cat.products_for_brand("Polymaker").size() == 2);
    CHECK(cat.products_for_type("PLA").size() == 2);
}

TEST_CASE_METHOD(HelixTestFixture, "user overlay overrides and adds", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_with_overlay(
        "tests/fixtures/filaments_test.json", "tests/fixtures/user_filaments_test.json");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 265);   // overridden by user
    CHECK(abs->nozzle_max == 285);
    const auto* added = cat.resolve_id("acme-custom-petg");
    REQUIRE(added != nullptr);       // new user product
    CHECK(added->brand == "Acme");
    CHECK(added->bed_temp == 80);    // inherited from PETG type
}

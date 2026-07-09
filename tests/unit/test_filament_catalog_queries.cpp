// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "helix_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include <algorithm>

using helix::printer::FilamentCatalog;
using helix::printer::EffectiveFilament;

namespace {
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}  // namespace

TEST_CASE_METHOD(HelixTestFixture, "all_brands is deduped and sorted", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_full();
    auto brands = cat.all_brands();
    REQUIRE(!brands.empty());
    // deduped
    auto sorted = brands;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    // "Generic" is present (seed products preserve it)
    REQUIRE(contains(brands, "Generic"));
}

TEST_CASE_METHOD(HelixTestFixture, "types_for_brand returns that brand's types only", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_full();
    auto types = cat.types_for_brand("Generic");
    REQUIRE(!types.empty());
    REQUIRE(contains(types, "PLA"));
    // every returned type actually has a Generic product
    for (const auto& t : types) {
        REQUIRE(!cat.products_for("Generic", t).empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "products_for filters by brand AND type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_full();
    auto prods = cat.products_for("Generic", "PLA");
    REQUIRE(!prods.empty());
    for (const auto* p : prods) {
        REQUIRE(p->brand == "Generic");
        REQUIRE(p->type == "PLA");
    }
}

TEST_CASE_METHOD(HelixTestFixture, "brands_for_type only lists carriers", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_full();
    auto brands = cat.brands_for_type("PLA");
    REQUIRE(contains(brands, "Generic"));
    for (const auto& b : brands) {
        REQUIRE(!cat.products_for(b, "PLA").empty());
    }
}

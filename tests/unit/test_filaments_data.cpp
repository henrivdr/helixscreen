// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "filament_database.h"
#include "helix_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include <set>

using helix::printer::FilamentCatalog;

TEST_CASE_METHOD(HelixTestFixture, "shipped filaments.json is well-formed", "[filament_data]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    std::set<std::string> ids;
    std::set<std::string> cfs_codes;
    for (const auto* p : all) {
        CHECK(ids.insert(p->id).second);                 // no duplicate ids
        CHECK(p->nozzle_min <= p->nozzle_recommended);
        CHECK(p->nozzle_recommended <= p->nozzle_max);
        // A product's type either resolves in filament_database.h (inherits
        // physical defaults) OR is a legitimately-unmapped Orca type — in which
        // case the importer emits explicit temps, so it must be self-sufficient.
        CHECK((filament::find_material(p->type).has_value() ||
               (p->nozzle_min > 0 && p->nozzle_max > 0)));
        auto it = p->codes.find("cfs");
        if (it != p->codes.end())
            CHECK(cfs_codes.insert(it->second).second);  // no dup cfs codes
    }
}

# Filament Picker (Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a non-Spoolman user pick a specific branded filament from the offline `FilamentCatalog` (via a Vendor→Type→product modal) and have it flow into an AMS slot and into a filament-panel preset.

**Architecture:** One reusable `FilamentCatalogPickerModal` (a `Modal` subclass) loads `FilamentCatalog::load_full()` for its open duration, presents two linked dropdowns (Vendor→Type) driving a C++-built product list, and emits one `EffectiveFilament` by value. Two consumers wire it in: the AMS edit modal (populates its live `working_info_`) and the preset long-press (persists a branded preset). The picker knows nothing about slots or presets.

**Tech Stack:** C++17, LVGL 9.5, helix-xml declarative UI, Catch2 tests, spdlog, nlohmann::json (via `hv/json.hpp`).

Spec: `docs/devel/specs/2026-07-03-filament-picker-phase2-design.md`.

## Global Constraints

Every task implicitly includes these (from CLAUDE.md + spec, verbatim values):

- **Build:** `make -j` (pure Makefile, never cmake/ninja). Tests: `make test` builds, `./build/bin/helix-tests "[tag]"` runs a tag. Run builds in the **foreground**; verify task completion via `git log`, not agent chat (Phase-1 subagents stalled backgrounding `make`).
- **XML loads at runtime** — XML-only changes need no rebuild, just relaunch. C++ changes need `make -j`.
- **SPDX header** on every new source file: `// SPDX-License-Identifier: GPL-3.0-or-later`.
- **Logging:** `spdlog` only (`spdlog::info/debug/warn/error`) — never `printf`/`cout`/`LV_LOG_*`.
- **Design tokens** in XML: colors `#card_bg`, spacing `#space_md`, semantic widgets `text_body`/`text_small`/`text_heading` — never hardcoded hex/px (L008).
- **Register every new XML component** in `src/xml_registration.cpp` via `register_xml("name.xml")` — unregistered = silent failure (L014).
- **Icon glyphs built in C++** (checkmarks etc.) MUST use the icon font, never `LV_SYMBOL_OK` (tofu on body font, L097). Resolve: `lv_xml_get_font(nullptr, lv_xml_get_const(nullptr, "icon_font_xs"))` + `ui_icon::lookup_codepoint("check")`.
- **Per-row identity** via `lv_obj_set_name(row, id)` / `lv_obj_get_name()` — NEVER `user_data` (reserved by `ui_button`/`text_input`; L069).
- **Async/bg callbacks** touching UI use `lifetime_.token()` + `tok.defer(...)` (L072). `Modal` provides `lifetime_`.
- **Widget deletion inside queued/observer/deferred callbacks:** never `lv_obj_clean`/`lv_obj_delete`/`safe_delete`; use `helix::ui::safe_clean_children()` / `lv_obj_delete_async()` (L059/L081).
- **`ObserverGuard` cleanup = `reset()`, never `release()`** (L085).
- **No test-only methods on production classes** — use `friend class FooTestAccess;` + a `tests/test_helpers/foo_test_access.h` (L065/L088).
- **`FilamentCatalog` is transient** — `load_full()`, query, drop. No resident singleton, zero idle RAM.
- **JSON guards:** default-constructed `nlohmann::json` is null; check `is_object()`/`is_string()`/`is_array()` before access (L087).
- **Dropdown order is Vendor → Type** (spec §2; overrides spec-1 §11's type-outer ordering).

---

## Decisions — RESOLVED (Preston, 2026-07-03)

1. **AMS entry point (Task 5) → DEFERRED.** Keep the existing AMS `material_dropdown` + `vendor_dropdown` as-is for now; do NOT wire the picker into the AMS edit modal this phase. The picker ships via the preset flow (Tasks 6-7). AMS catalog integration becomes a documented Phase-2 follow-up. **Task 5 is not implemented — see its section for the deferral note.**
2. **Preset persistence (Task 6) → CONFIG MIGRATION** (spec §7.2 as written), not the additive key. Grow `/preset_materials` from a 4-string array to a 4-object array via a versioned migration (`CURRENT_CONFIG_VERSION` 18→19, `migrate_v18_to_v19`). One key, cleaner long-term. **Task 6 rewritten below for the migration approach.**

---

## File Structure

**New:**
- `include/ui_filament_catalog_picker.h` — `FilamentCatalogPickerModal` class (Modal subclass).
- `src/ui/ui_filament_catalog_picker.cpp` — its implementation.
- `ui_xml/components/filament_catalog_picker.xml` — the modal layout.
- `tests/unit/test_filament_catalog_queries.cpp` — catalog query API tests (`[filament_catalog]`).
- `tests/unit/test_preset_filament_persistence.cpp` — preset branded persistence tests (`[material_settings]`).
- `tests/test_helpers/filament_picker_test_access.h` — friend accessor for the modal's private query helpers (if needed).

**Modified:**
- `include/filament_catalog.h` / `src/printer/filament_catalog.cpp` — new queries + minors (Task 1).
- `src/xml_registration.cpp` — register picker; remove dead-modal registration (Tasks 2, 8).
- `include/ui_ams_edit_modal.h` / `src/ui/ui_ams_edit_modal.cpp` / `ui_xml/ams_edit_modal.xml` — AMS integration (Task 5).
- `include/material_settings_manager.h` / `src/system/material_settings_manager.cpp` — branded preset persistence (Task 6).
- `include/ui_panel_filament.h` / `src/ui/ui_panel_filament.cpp` — preset long-press wiring (Task 7).

**Deleted (Task 8):**
- `ui_xml/filament_preset_edit_modal.xml` + its `register_xml` line.
- `include/ui_material_picker_menu.h` / `src/ui/ui_material_picker_menu.cpp` / `ui_xml/material_picker_menu.xml` + registration (retired; only caller is the preset long-press, rewired in Task 7).

---

# STAGE 2a — Catalog API + Picker Modal + AMS Integration

## Task 1: `FilamentCatalog` query API + Phase-1 minors

**Files:**
- Modify: `include/filament_catalog.h`, `src/printer/filament_catalog.cpp`
- Test: `tests/unit/test_filament_catalog_queries.cpp` (new)

**Interfaces:**
- Consumes: existing `FilamentCatalog::load_full()`, `all_products()`, `EffectiveFilament`.
- Produces (later tasks depend on these exact signatures):
  - `std::vector<std::string> types_for_brand(const std::string& brand) const;`
  - `std::vector<std::string> brands_for_type(const std::string& type) const;`
  - `std::vector<const EffectiveFilament*> products_for(const std::string& brand, const std::string& type) const;`
  - `all_brands()` returns deduped, sorted, "Generic" not special-cased here (caller pins it).

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_filament_catalog_queries.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "helix_test_fixture.h"
#include <catch2/catch_test_macros.hpp>
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
```

- [ ] **Step 2: Add the test file to the build and run to confirm it fails**

The unit test glob should auto-include it. Run:
```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[filament_catalog]" 2>&1 | tail -30
```
Expected: compile error (`types_for_brand` not a member) or link failure — the methods don't exist yet.

- [ ] **Step 3: Declare the new methods + `=delete` copy-ctor in the header**

In `include/filament_catalog.h`, inside `class FilamentCatalog`, add to the public section (next to `products_for_type`):
```cpp
    std::vector<std::string> types_for_brand(const std::string& brand) const;
    std::vector<std::string> brands_for_type(const std::string& type) const;
    std::vector<const EffectiveFilament*> products_for(const std::string& brand,
                                                       const std::string& type) const;
```
And below the constructors region (the class currently has an implicit copy-ctor). Add, right after the class opening `public:` or near the loaders:
```cpp
    FilamentCatalog(const FilamentCatalog&) = delete;
    FilamentCatalog& operator=(const FilamentCatalog&) = delete;
    FilamentCatalog(FilamentCatalog&&) = default;
    FilamentCatalog& operator=(FilamentCatalog&&) = default;
```
(The static `load_*` factories return by value → they need the move ops once copy is deleted.)

- [ ] **Step 4: Implement the queries in the .cpp**

In `src/printer/filament_catalog.cpp`, add:
```cpp
std::vector<std::string> FilamentCatalog::types_for_brand(const std::string& brand) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.brand == brand && seen.insert(p.type).second) {
            out.push_back(p.type);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> FilamentCatalog::brands_for_type(const std::string& type) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& p : products_) {
        if (p.type == type && seen.insert(p.brand).second) {
            out.push_back(p.brand);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<const EffectiveFilament*> FilamentCatalog::products_for(
    const std::string& brand, const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& p : products_) {
        if (p.brand == brand && p.type == type) {
            out.push_back(&p);
        }
    }
    return out;
}
```
Ensure `#include <set>` and `#include <algorithm>` are present at the top.

- [ ] **Step 5: Fix `all_brands()` dedup (O(n²) → set) and restore `load_full()` fallthrough**

Rewrite `all_brands()` in `src/printer/filament_catalog.cpp` to:
```cpp
std::vector<std::string> FilamentCatalog::all_brands() const {
    std::set<std::string> seen;
    for (const auto& p : products_) seen.insert(p.brand);
    return {seen.begin(), seen.end()};  // sorted + deduped
}
```
For `load_full()`: locate the overlay-load path (grep `load_with_overlay` / the overlay branch) and confirm a malformed/missing overlay path logs and continues to return the built-in catalog rather than aborting. If Phase 1 dropped the per-path parse-fallthrough, wrap the overlay parse in a `try/catch` that `spdlog::warn`s and proceeds. (If the built-in load already tolerates a missing overlay, note it and skip.)

- [ ] **Step 6: Build and run the tests**
```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[filament_catalog]" 2>&1 | tail -30
```
Expected: all 4 `[filament_catalog]` tests PASS. Also run the existing data lint to ensure no regression:
```bash
./build/bin/helix-tests "[filament_data]" 2>&1 | tail -10
```

- [ ] **Step 7: Commit**
```bash
git add include/filament_catalog.h src/printer/filament_catalog.cpp tests/unit/test_filament_catalog_queries.cpp
git commit -m "feat(filament): FilamentCatalog vendor/type query API + dedup/copy-ctor minors"
```

---

## Task 2: Picker modal skeleton — XML + class + Vendor dropdown

**Files:**
- Create: `ui_xml/components/filament_catalog_picker.xml`, `include/ui_filament_catalog_picker.h`, `src/ui/ui_filament_catalog_picker.cpp`
- Modify: `src/xml_registration.cpp`
- Test: extend `tests/unit/test_filament_catalog_queries.cpp` or new `tests/unit/test_filament_picker_modal.cpp` (uses `XMLTestFixture`)

**Interfaces:**
- Consumes: Task 1 (`load_full`, `all_brands`, `types_for_brand`, `products_for`), `Modal` base (`include/ui_modal.h`), `helix::printer::EffectiveFilament`.
- Produces:
  - `class FilamentCatalogPickerModal : public helix::ui::Modal`
  - `using SelectCallback = std::function<void(const helix::printer::EffectiveFilament&)>;`
  - `void show(lv_obj_t* parent, std::optional<std::string> seed_type, SelectCallback on_select);`
  - `void show(lv_obj_t* parent, std::optional<std::string> seed_type, std::optional<std::vector<std::string>> allowed_types, SelectCallback on_select);` (allowed_types filters the Type dropdown — used by AMS in Task 5)
  - Component name `"filament_catalog_picker"`.

- [ ] **Step 1: Write the XML component**

Create `ui_xml/components/filament_catalog_picker.xml` (mirror `spoolman_edit_modal.xml` structure; `ipp_print_modal.xml` for dropdown rows). Dropdowns are bare (filled from C++); the product list is a named scrollable container:
```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Offline branded-filament picker: Vendor -> Type -> product list -->
<component>
  <view name="filament_catalog_picker"
        extends="ui_dialog" width="70%" height="content" style_min_width="380"
        align="center" flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
    <modal_header title="Choose Filament" title_tag="Choose Filament"
                  hide_icon="true" hide_close="false" close_callback="catalog_picker_close_cb"/>
    <divider_horizontal/>
    <lv_obj width="100%" height="content" style_pad_all="#space_md" flex_flow="column"
            style_pad_gap="#space_md" scrollable="false">
      <!-- Vendor row -->
      <lv_obj width="100%" height="content" style_pad_all="0" flex_flow="row"
              style_flex_cross_place="center" style_pad_gap="#space_md" scrollable="false">
        <text_small text="Vendor" translation_tag="Vendor" width="60"/>
        <lv_dropdown name="vendor_dropdown" flex_grow="1" height="content"
                     style_radius="#border_radius">
          <event_cb trigger="value_changed" callback="catalog_picker_vendor_changed_cb"/>
        </lv_dropdown>
      </lv_obj>
      <!-- Type row -->
      <lv_obj width="100%" height="content" style_pad_all="0" flex_flow="row"
              style_flex_cross_place="center" style_pad_gap="#space_md" scrollable="false">
        <text_small text="Type" translation_tag="Type" width="60"/>
        <lv_dropdown name="type_dropdown" flex_grow="1" height="content"
                     style_radius="#border_radius">
          <event_cb trigger="value_changed" callback="catalog_picker_type_changed_cb"/>
        </lv_dropdown>
      </lv_obj>
      <!-- Product list (rows built in C++) -->
      <lv_obj name="product_list" width="100%" height="200" style_pad_all="0"
              flex_flow="column" style_pad_gap="#space_xxs"/>
    </lv_obj>
    <modal_button_row primary_text="Select" primary_tag="Select"
                      primary_callback="catalog_picker_select_cb"
                      secondary_text="Cancel" secondary_tag="Cancel"
                      secondary_callback="catalog_picker_close_cb"/>
  </view>
</component>
```

- [ ] **Step 2: Write the header**

Create `include/ui_filament_catalog_picker.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "filament_catalog.h"
#include "ui_modal.h"

namespace helix::ui {

/// Offline branded-filament picker. Loads FilamentCatalog::load_full() for its open
/// duration (transient — freed on hide), presents linked Vendor->Type dropdowns driving
/// a product list, and emits one EffectiveFilament by value. Knows nothing about slots
/// or presets — callers decide what to do with the result.
class FilamentCatalogPickerModal : public Modal {
  public:
    using SelectCallback = std::function<void(const helix::printer::EffectiveFilament&)>;

    FilamentCatalogPickerModal() = default;
    ~FilamentCatalogPickerModal() override = default;
    FilamentCatalogPickerModal(const FilamentCatalogPickerModal&) = delete;
    FilamentCatalogPickerModal& operator=(const FilamentCatalogPickerModal&) = delete;

    /// seed_type: pre-select the Type dropdown (preset path); nullopt = free (AMS path).
    void show(lv_obj_t* parent, std::optional<std::string> seed_type, SelectCallback on_select);
    /// allowed_types: restrict the Type dropdown to this set (AMS backend whitelist).
    void show(lv_obj_t* parent, std::optional<std::string> seed_type,
              std::optional<std::vector<std::string>> allowed_types, SelectCallback on_select);

    [[nodiscard]] const char* get_name() const override { return "Filament Catalog Picker"; }
    [[nodiscard]] const char* component_name() const override { return "filament_catalog_picker"; }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    void populate_vendor_dropdown();
    void populate_type_dropdown();      // from current vendor (+ allowed_types filter)
    void rebuild_product_list();        // from current vendor+type
    void handle_vendor_changed();
    void handle_type_changed();
    void handle_row_selected(const std::string& product_id);
    void handle_select_button();        // confirm current highlighted row

    std::string current_vendor() const; // reads vendor_dropdown selected string
    std::string current_type() const;   // reads type_dropdown selected string

    static void register_callbacks();
    static bool callbacks_registered_;
    static FilamentCatalogPickerModal* active_instance_;

    helix::printer::FilamentCatalog catalog_ = helix::printer::FilamentCatalog::load_full();
    std::optional<std::string> seed_type_;
    std::optional<std::vector<std::string>> allowed_types_;
    SelectCallback on_select_;
    std::string highlighted_id_;        // currently-selected product row id
    std::vector<std::string> vendor_order_;  // dropdown index -> brand (Generic pinned first)
};

}  // namespace helix::ui
```
Note: `catalog_` is loaded at construction. Because the modal is created fresh per `show()` OR reloaded — see Step 5 (reload in `show()` so a re-open sees overlay edits and frees between opens).

- [ ] **Step 3: Write the .cpp skeleton (show / callbacks / vendor dropdown)**

Create `src/ui/ui_filament_catalog_picker.cpp`. This step: statics, `register_callbacks`, `show`, `on_show` (populate vendor), `on_hide`, and vendor-dropdown population. (Type + product list land in Task 3.)
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_picker.h"

#include <spdlog/spdlog.h>

#include "lvgl.h"

namespace helix::ui {

bool FilamentCatalogPickerModal::callbacks_registered_ = false;
FilamentCatalogPickerModal* FilamentCatalogPickerModal::active_instance_ = nullptr;

void FilamentCatalogPickerModal::register_callbacks() {
    if (callbacks_registered_) return;
    lv_xml_register_event_cb(nullptr, "catalog_picker_close_cb",
        [](lv_event_t*) { if (active_instance_) active_instance_->hide(); });
    lv_xml_register_event_cb(nullptr, "catalog_picker_vendor_changed_cb",
        [](lv_event_t*) { if (active_instance_) active_instance_->handle_vendor_changed(); });
    lv_xml_register_event_cb(nullptr, "catalog_picker_type_changed_cb",
        [](lv_event_t*) { if (active_instance_) active_instance_->handle_type_changed(); });
    lv_xml_register_event_cb(nullptr, "catalog_picker_select_cb",
        [](lv_event_t*) { if (active_instance_) active_instance_->handle_select_button(); });
    callbacks_registered_ = true;
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      SelectCallback on_select) {
    show(parent, std::move(seed_type), std::nullopt, std::move(on_select));
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      std::optional<std::vector<std::string>> allowed_types,
                                      SelectCallback on_select) {
    register_callbacks();
    catalog_ = helix::printer::FilamentCatalog::load_full();  // fresh load per open
    seed_type_ = std::move(seed_type);
    allowed_types_ = std::move(allowed_types);
    on_select_ = std::move(on_select);
    highlighted_id_.clear();
    Modal::show(parent);   // triggers on_show()
}

void FilamentCatalogPickerModal::on_show() {
    active_instance_ = this;
    populate_vendor_dropdown();
    populate_type_dropdown();
    rebuild_product_list();
}

void FilamentCatalogPickerModal::on_hide() {
    if (active_instance_ == this) active_instance_ = nullptr;
    // catalog_ stays until next show() reloads it; memory is bounded to one catalog.
}

std::string FilamentCatalogPickerModal::current_vendor() const {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "vendor_dropdown");
    if (!dd) return {};
    uint32_t sel = lv_dropdown_get_selected(dd);
    return sel < vendor_order_.size() ? vendor_order_[sel] : std::string{};
}

std::string FilamentCatalogPickerModal::current_type() const {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "type_dropdown");
    if (!dd) return {};
    char buf[64] = {};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    return buf;
}

void FilamentCatalogPickerModal::populate_vendor_dropdown() {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "vendor_dropdown");
    if (!dd) return;
    // "Generic" pinned first, then the rest (all_brands() is already sorted+deduped).
    vendor_order_.clear();
    vendor_order_.push_back("Generic");
    for (const auto& b : catalog_.all_brands()) {
        if (b != "Generic") vendor_order_.push_back(b);
    }
    std::string options;
    for (size_t i = 0; i < vendor_order_.size(); ++i) {
        if (i) options += "\n";
        options += vendor_order_[i];
    }
    lv_dropdown_set_options(dd, options.c_str());
    lv_dropdown_set_selected(dd, 0);  // Generic
}

// populate_type_dropdown / rebuild_product_list / handle_* stubbed here, filled in Task 3-4.
void FilamentCatalogPickerModal::populate_type_dropdown() {}
void FilamentCatalogPickerModal::rebuild_product_list() {}
void FilamentCatalogPickerModal::handle_vendor_changed() {}
void FilamentCatalogPickerModal::handle_type_changed() {}
void FilamentCatalogPickerModal::handle_row_selected(const std::string&) {}
void FilamentCatalogPickerModal::handle_select_button() {}

}  // namespace helix::ui
```

- [ ] **Step 4: Register the component + add to build**

In `src/xml_registration.cpp`, add near the other component registrations (e.g. after `register_xml("spoolman_edit_modal.xml");`):
```cpp
register_xml("filament_catalog_picker.xml");
```
The `src/ui/*.cpp` glob picks up the new source automatically. Confirm the source is in the build (grep the Makefile/`mk/*.mk` for how `src/ui` is globbed — it's wildcard-based; no manual add needed).

- [ ] **Step 5: Build and smoke-test that the modal opens**
```bash
make -j 2>&1 | tail -20
```
Expected: clean build. (Interactive open is verified in Task 3; here just confirm compile + registration.)

- [ ] **Step 6: Commit**
```bash
git add include/ui_filament_catalog_picker.h src/ui/ui_filament_catalog_picker.cpp \
        ui_xml/components/filament_catalog_picker.xml src/xml_registration.cpp
git commit -m "feat(filament): FilamentCatalogPickerModal skeleton + vendor dropdown"
```

---

## Task 3: Linked Type dropdown + product list rows

**Files:**
- Modify: `src/ui/ui_filament_catalog_picker.cpp`

**Interfaces:**
- Consumes: Task 2 members; Task 1 queries; icon-font pattern from `src/ui/ui_material_picker_menu.cpp:87-135`.
- Produces: working linkage — vendor change refills type + list; type change refills list; rows clickable and highlightable.

- [ ] **Step 1: Implement `populate_type_dropdown()` (vendor-filtered + allowed_types + seed)**

Replace the stub:
```cpp
void FilamentCatalogPickerModal::populate_type_dropdown() {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "type_dropdown");
    if (!dd) return;
    std::vector<std::string> types = catalog_.types_for_brand(current_vendor());
    if (allowed_types_) {
        std::vector<std::string> filtered;
        for (const auto& t : types) {
            if (std::find(allowed_types_->begin(), allowed_types_->end(), t) != allowed_types_->end())
                filtered.push_back(t);
        }
        types.swap(filtered);
    }
    std::string options;
    int seed_idx = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i) options += "\n";
        options += types[i];
        if (seed_type_ && types[i] == *seed_type_) seed_idx = static_cast<int>(i);
    }
    lv_dropdown_set_options(dd, options.empty() ? "" : options.c_str());
    lv_dropdown_set_selected(dd, seed_idx);
}
```
Add `#include <algorithm>` at the top.

- [ ] **Step 2: Implement `rebuild_product_list()` with icon-font rows**

Mirror `ui_material_picker_menu.cpp:87-135` verbatim for font/glyph resolution and row construction. Use `safe_clean_children` for the rebuild (L081):
```cpp
void FilamentCatalogPickerModal::rebuild_product_list() {
    lv_obj_t* list = lv_obj_find_by_name(dialog(), "product_list");
    if (!list) return;
    helix::ui::safe_clean_children(list);

    lv_color_t accent = theme_manager_get_color("primary");
    lv_color_t text_color = theme_manager_get_color("text");
    const char* body_font_name = lv_xml_get_const(nullptr, "font_body");
    const lv_font_t* body_font =
        body_font_name ? lv_xml_get_font(nullptr, body_font_name) : lv_font_get_default();
    const char* icon_font_name = lv_xml_get_const(nullptr, "icon_font_xs");
    const lv_font_t* icon_font =
        icon_font_name ? lv_xml_get_font(nullptr, icon_font_name) : body_font;
    const char* check_codepoint = ui_icon::lookup_codepoint("check");

    auto products = catalog_.products_for(current_vendor(), current_type());
    for (const auto* p : products) {
        bool is_current = (highlighted_id_ == p->id);
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, accent, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_name(row, p->id.c_str());  // identity for click handler (L069)

        lv_obj_t* indicator = lv_label_create(row);
        lv_obj_set_style_text_font(indicator, icon_font, 0);
        lv_obj_set_style_min_width(indicator, 16, 0);
        if (is_current && check_codepoint) {
            lv_label_set_text(indicator, check_codepoint);
            lv_obj_set_style_text_color(indicator, accent, 0);
        } else {
            lv_label_set_text(indicator, "");
        }
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, p->name.c_str());
        lv_obj_set_style_text_font(name_lbl, body_font, 0);
        lv_obj_set_style_text_color(name_lbl, is_current ? accent : text_color, 0);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_obj_remove_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

        char temps[32];
        snprintf(temps, sizeof(temps), "%d° / %d°", p->nozzle_recommended, p->bed_temp);
        lv_obj_t* temp_lbl = lv_label_create(row);
        lv_label_set_text(temp_lbl, temps);
        lv_obj_set_style_text_font(temp_lbl, body_font, 0);
        lv_obj_set_style_text_color(temp_lbl, text_color, 0);
        lv_obj_remove_flag(temp_lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            lv_obj_t* row = lv_event_get_current_target_obj(e);
            if (!row || !active_instance_) return;
            const char* id = lv_obj_get_name(row);
            if (id) active_instance_->handle_row_selected(id);
        }, LV_EVENT_CLICKED, nullptr);
    }
}
```
Add includes: `"theme_manager.h"` (or the header exposing `theme_manager_get_color`), `"ui_icon_codepoints.h"`, `"ui_utils.h"` (for `safe_clean_children`), and whatever declares `lv_xml_get_font`/`lv_xml_get_const`. Copy the exact include set from `ui_material_picker_menu.cpp`.

- [ ] **Step 3: Implement the change handlers + row highlight**
```cpp
void FilamentCatalogPickerModal::handle_vendor_changed() {
    populate_type_dropdown();  // vendor changed -> types change
    rebuild_product_list();
}

void FilamentCatalogPickerModal::handle_type_changed() {
    rebuild_product_list();
}

void FilamentCatalogPickerModal::handle_row_selected(const std::string& product_id) {
    highlighted_id_ = product_id;
    rebuild_product_list();  // redraw to move the checkmark
}
```

- [ ] **Step 4: Build**
```bash
make -j 2>&1 | tail -20
```
Expected: clean build.

- [ ] **Step 5: Interactive verify (checkmark + linkage — unit tests can't catch tofu, L097)**

Run headless with the mock printer and drive to the AMS/preset flow once Task 5/7 wires an entry — OR add a temporary debug launch. For now, verify the build and defer interactive glyph check to Task 5's verify step. Note in the commit that interactive verification is pending an entry point.

- [ ] **Step 6: Commit**
```bash
git add src/ui/ui_filament_catalog_picker.cpp
git commit -m "feat(filament): picker linked type dropdown + product-list rows"
```

---

## Task 4: Selection → emit `EffectiveFilament`

**Files:**
- Modify: `src/ui/ui_filament_catalog_picker.cpp`
- Test: `tests/unit/test_filament_picker_modal.cpp` (new, `XMLTestFixture`)

**Interfaces:**
- Consumes: Task 3.
- Produces: `handle_select_button()` resolves `highlighted_id_` via `catalog_.resolve_id()` and invokes `on_select_` with a by-value copy, then hides.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_filament_picker_modal.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_picker.h"
#include "xml_test_fixture.h"
#include <catch2/catch_test_macros.hpp>

using helix::ui::FilamentCatalogPickerModal;
using helix::printer::EffectiveFilament;

TEST_CASE_METHOD(XMLTestFixture, "picker emits selected EffectiveFilament", "[filament_picker]") {
    FilamentCatalogPickerModal modal;
    std::optional<EffectiveFilament> got;
    modal.show(lv_screen_active(), std::string("PLA"),
               [&](const EffectiveFilament& ef) { got = ef; });
    // Drive: select the first Generic PLA product via the test accessor, then confirm.
    helix::ui::FilamentPickerTestAccess::select_first_product(modal);
    helix::ui::FilamentPickerTestAccess::press_select(modal);
    helix::UpdateQueue::instance().drain_queue_for_testing();
    REQUIRE(got.has_value());
    REQUIRE(got->type == "PLA");
    REQUIRE(!got->brand.empty());
}
```
Create `tests/test_helpers/filament_picker_test_access.h` (friend accessor, L065):
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui_filament_catalog_picker.h"
namespace helix::ui {
struct FilamentPickerTestAccess {
    static void select_first_product(FilamentCatalogPickerModal& m);  // sets highlighted_id_ to first row
    static void press_select(FilamentCatalogPickerModal& m) { m.handle_select_button(); }
};
}
```
Add `friend struct FilamentPickerTestAccess;` to the modal's private section. Implement `select_first_product` in the test-helper .cpp or inline (reads `m.catalog_.products_for(m.current_vendor(), m.current_type())` first element → `m.highlighted_id_ = ...; m.rebuild_product_list();`).

- [ ] **Step 2: Run test to confirm it fails**
```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[filament_picker]" 2>&1 | tail -20
```
Expected: FAIL — `handle_select_button` is a no-op, `got` stays empty.

- [ ] **Step 3: Implement `handle_select_button()`**
```cpp
void FilamentCatalogPickerModal::handle_select_button() {
    if (highlighted_id_.empty()) {
        spdlog::debug("[FilamentCatalogPicker] Select pressed with no product highlighted");
        return;  // no-op; user must pick a row first
    }
    const EffectiveFilament* ef = catalog_.resolve_id(highlighted_id_);
    if (ef && on_select_) {
        EffectiveFilament copy = *ef;  // by value — nothing dangles after catalog_ reloads
        on_select_(copy);
    }
    hide();
}
```

- [ ] **Step 4: Run test to confirm it passes**
```bash
make test 2>&1 | tail -20
./build/bin/helix-tests "[filament_picker]" 2>&1 | tail -20
```
Expected: PASS.

- [ ] **Step 5: Commit**
```bash
git add src/ui/ui_filament_catalog_picker.cpp tests/unit/test_filament_picker_modal.cpp \
        tests/test_helpers/filament_picker_test_access.h include/ui_filament_catalog_picker.h
git commit -m "feat(filament): picker Select emits EffectiveFilament"
```

---

## Task 5: AMS edit modal integration

**Files:**
- Modify: `include/ui_ams_edit_modal.h`, `src/ui/ui_ams_edit_modal.cpp`, `ui_xml/ams_edit_modal.xml`

**Interfaces:**
- Consumes: `FilamentCatalogPickerModal` (Task 4), `AmsEditModal::working_info_` (`SlotInfo`), `handle_spool_selected` pattern (`ui_ams_edit_modal.cpp:693-732`).
- Produces: tapping the material control opens the picker; selecting populates `working_info_` and refreshes via `update_ui()`.

> **⛔ DEFERRED (Preston, 2026-07-03) — NOT implemented this phase.** Keep the existing AMS `material_dropdown` + `vendor_dropdown` as-is for now. The picker ships via the preset flow (Tasks 6-7); wiring it into the AMS edit modal is a Phase-2 follow-up. The steps below are retained as the reference design for that follow-up — **do not execute them now.** (The picker already carries the `allowed_types` param needed for the AMS whitelist, so no rework of the picker is required when this is picked up.)

- [ ] **Step 1: Add the picker member + a handler to the header**

In `include/ui_ams_edit_modal.h`, add an include `#include "ui_filament_catalog_picker.h"`, a member near `spool_edit_modal_`:
```cpp
    FilamentCatalogPickerModal catalog_picker_;
```
and a private method:
```cpp
    void handle_open_catalog_picker();
    void apply_catalog_filament(const helix::printer::EffectiveFilament& ef);
```

- [ ] **Step 2: Add the launch handler + apply logic (.cpp)**

Mirror `handle_spool_selected` (`ui_ams_edit_modal.cpp:693-732`) for the apply. Add:
```cpp
void AmsEditModal::handle_open_catalog_picker() {
    // Restrict picker types to the backend's allowed materials (material_list_).
    auto token = lifetime_.token();
    catalog_picker_.show(
        lv_obj_get_screen(dialog()),
        working_info_.material.empty() ? std::nullopt : std::optional<std::string>(working_info_.material),
        material_list_.empty() ? std::nullopt : std::optional<std::vector<std::string>>(material_list_),
        [this, token](const helix::printer::EffectiveFilament& ef) {
            if (token.expired()) return;
            token.defer("AmsEditModal::apply_catalog_filament",
                        [this, ef]() { apply_catalog_filament(ef); });
        });
}

void AmsEditModal::apply_catalog_filament(const helix::printer::EffectiveFilament& ef) {
    working_info_.material = ef.type;
    working_info_.brand = ef.brand;
    working_info_.spool_name = ef.brand + " " + ef.name;
    working_info_.nozzle_temp_min = ef.nozzle_min;
    working_info_.nozzle_temp_max = ef.nozzle_max;
    working_info_.bed_temp = ef.bed_temp;
    filament_user_edited_ = true;
    switch_to_form();
    update_ui();
    update_sync_button_state();
    spdlog::info("[AmsEditModal] Applied catalog filament {} {} to slot {}",
                 ef.brand, ef.name, slot_index_);
}
```
(`token.defer` is correct here — the callback fires on the main thread from the picker's `on_select_`; using the token guards against the AmsEditModal being dismissed first. L072.)

- [ ] **Step 3: Wire the entry point in XML + register the callback**

In `ui_xml/ams_edit_modal.xml`, replace the `material_dropdown` block (`:128-136`) with a tappable field. Keep the label; make the value a button that opens the picker:
```xml
<!-- Material (opens catalog picker) -->
<lv_obj height="content" flex_grow="3" style_pad_all="0" flex_flow="column"
        style_pad_gap="#space_xxs" scrollable="false">
  <text_small text="Material" translation_tag="Material"/>
  <ui_button name="material_picker_btn" width="100%" height="content"
             style_radius="#border_radius">
    <text_small name="material_picker_label" text="PLA"/>
    <event_cb trigger="clicked" callback="ams_edit_open_catalog_cb"/>
  </ui_button>
</lv_obj>
```
Register the callback in `AmsEditModal::register_callbacks` (find the existing registration block, add):
```cpp
lv_xml_register_event_cb(nullptr, "ams_edit_open_catalog_cb", on_open_catalog_cb);
```
Add the static shim (mirror `on_material_changed_cb`):
```cpp
void AmsEditModal::on_open_catalog_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) self->handle_open_catalog_picker();
}
```
Declare `static void on_open_catalog_cb(lv_event_t* e);` in the header.

- [ ] **Step 4: Update `update_ui()` to set the new label instead of the dropdown**

In `update_ui()` replace the `material_dropdown` block (`ui_ams_edit_modal.cpp:1151-1170`) with:
```cpp
lv_obj_t* material_label = find_widget("material_picker_label");
if (material_label) {
    std::string txt = working_info_.brand.empty() || working_info_.brand == "Generic"
        ? working_info_.material
        : working_info_.brand + " " + working_info_.material;
    lv_label_set_text(material_label, txt.empty() ? "PLA" : txt.c_str());
}
```
Remove now-dead `material_dropdown`/`material_options_`/`handle_material_changed` dropdown plumbing only if fully unused — otherwise leave `material_list_` (the picker's allowed-types source still needs it built; keep the `material_list_` population at `:1041-1101`).

- [ ] **Step 5: Build + interactive verify**
```bash
make -j 2>&1 | tail -20
./build/bin/helix-screen --test -vv
```
Manually: open an AMS slot edit → tap the Material field → picker opens → change Vendor → Type list updates → pick a branded product → Select → confirm the modal's material label + temps update and the checkmark rendered (not tofu, L097). Report what you saw.

- [ ] **Step 6: Commit**
```bash
git add include/ui_ams_edit_modal.h src/ui/ui_ams_edit_modal.cpp ui_xml/ams_edit_modal.xml
git commit -m "feat(filament): wire catalog picker into AMS slot edit"
```

**End of Stage 2a — the reusable picker core (Tasks 1-4) is complete. AMS integration (Task 5) is deferred to a Phase-2 follow-up per Preston's decision; the picker ships this phase via the preset flow (Stage 2b).**

---

# STAGE 2b — Preset branded upgrade

## Task 6: `MaterialSettingsManager` branded preset persistence (config migration)

**Approach (Preston's decision):** Grow the single `/preset_materials` key from a 4-string array to a 4-object array via a **versioned config migration** (`migrate_v18_to_v19`). No second key. In memory, keep `preset_materials_` (the 4 type strings — `get_preset_materials()` and existing callers stay working) and add a parallel `preset_filaments_` array for branding; both serialize together into the object array.

**On-disk format after migration** (`/preset_materials`):
```jsonc
[ { "type": "PLA" },                         // generic slot
  { "type": "PLA", "filament_id": "orca_bambu_pla_matte",
    "brand": "Bambu Lab", "name": "PLA Matte", "nozzle": 220, "bed": 55 },  // branded
  { "type": "ABS" }, { "type": "TPU" } ]
```

**Files:**
- Modify: `include/material_settings_manager.h`, `src/system/material_settings_manager.cpp`, `include/config.h` (version bump), `src/system/config.cpp` (migration)
- Test: `tests/unit/test_preset_filament_persistence.cpp` (new)

**Interfaces produced (Task 7 depends on these exact signatures):**
- `struct PresetFilament { std::string filament_id, brand, name; int nozzle = 0; int bed = 0; bool is_branded() const { return !filament_id.empty(); } };` (public nested in `MaterialSettingsManager`)
- `std::optional<PresetFilament> get_preset_filament(int index) const;`
- `void set_preset_filament(int index, const helix::printer::EffectiveFilament& ef);`
- `void clear_preset_filament(int index);`
- `get_preset_materials()` **unchanged** — still returns `std::array<std::string,4>` of types.
- `set_preset_material(int, string)` also clears that slot's branded entry.

**Reference (exact current code — from a verified read):**
- `/preset_materials` today: 4-string array. Load `material_settings_manager.cpp:124-149` (`load_presets_from_config`), save `:151-162` (`save_presets_to_config`), setter `set_preset_material` `:164-171`, defaults `DEFAULT_PRESET_MATERIALS` (`.h:14`), member `preset_materials_` (`.h:77`). `init()` calls `load_presets_from_config()`.
- Migration: `CURRENT_CONFIG_VERSION = 18` (`config.h:58`). Chain in `run_versioned_migrations()` (`config.cpp:799-844`), each `static void migrate_vN_to_vN1(json&)` in the anon namespace, registered as `if (version < N1) migrate_vN_to_vN1(config);` before the final `config["config_version"] = CURRENT_CONFIG_VERSION;` (`config.cpp:843`). Template to mirror: `migrate_v17_to_v18` (`config.cpp:774-797`) — guards with `contains()`/type checks, logs `spdlog::info("[Config] Migration vN: ...")`, idempotent. Migration test template: `tests/unit/test_config_migration_v18.cpp`.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_preset_filament_persistence.cpp`. **Use the repo's Catch include `"../catch_amalgamated.hpp"` (NOT `<catch2/...>`)** — verify against a sibling test's first lines:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "material_settings_manager.h"
#include "filament_catalog.h"
#include "config.h"
#include "helix_test_fixture.h"
#include "../catch_amalgamated.hpp"

using helix::MaterialSettingsManager;

TEST_CASE_METHOD(HelixTestFixture, "preset filament round-trips via config", "[material_settings]") {
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    helix::printer::EffectiveFilament ef;
    ef.id = "orca_test_pla"; ef.brand = "Bambu Lab"; ef.name = "PLA Matte";
    ef.type = "PLA"; ef.nozzle_recommended = 220; ef.bed_temp = 55;

    mgr.set_preset_filament(1, ef);
    auto got = mgr.get_preset_filament(1);
    REQUIRE(got.has_value());
    REQUIRE(got->filament_id == "orca_test_pla");
    REQUIRE(got->brand == "Bambu Lab");
    REQUIRE(got->nozzle == 220);
    REQUIRE(got->bed == 55);
    // type kept in lockstep, slot 0 stays generic
    REQUIRE(mgr.get_preset_materials()[1] == "PLA");
    REQUIRE_FALSE(mgr.get_preset_filament(0).has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "set_preset_material clears branding for that slot", "[material_settings]") {
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    helix::printer::EffectiveFilament ef;
    ef.id = "x"; ef.brand = "B"; ef.name = "N"; ef.type = "PETG"; ef.nozzle_recommended = 240; ef.bed_temp = 70;
    mgr.set_preset_filament(2, ef);
    REQUIRE(mgr.get_preset_filament(2).has_value());
    mgr.set_preset_material(2, "ABS");     // plain type-swap reverts to generic
    REQUIRE_FALSE(mgr.get_preset_filament(2).has_value());
    REQUIRE(mgr.get_preset_materials()[2] == "ABS");
}

TEST_CASE_METHOD(HelixTestFixture, "load tolerates legacy bare-string preset_materials", "[material_settings]") {
    // Defensive: if /preset_materials still holds bare strings (pre-migration or hand-edit),
    // load must treat each string as {type: string}, not crash.
    Config* config = Config::get_instance();
    REQUIRE(config != nullptr);
    config->get_json("/preset_materials") = nlohmann::json::array({"PLA", "PETG", "ABS", "TPU"});
    auto& mgr = MaterialSettingsManager::instance();
    mgr.init();
    REQUIRE(mgr.get_preset_materials()[0] == "PLA");
    REQUIRE(mgr.get_preset_materials()[3] == "TPU");
    for (int i = 0; i < 4; ++i) REQUIRE_FALSE(mgr.get_preset_filament(i).has_value());
}
```

**Also add a migration test** — mirror `tests/unit/test_config_migration_v18.cpp` exactly (same harness for invoking the versioned migration on a hand-built `config_version:18` JSON). Assert: a v18 config whose `/preset_materials` is `["PLA","PETG","ABS","TPU"]` becomes, after migration, an array of 4 OBJECTS each with `type` set to the original string (`[{ "type":"PLA" }, ...]`), and `config_version` becomes 19. Add an idempotency assertion: running it again (or on an already-object array) leaves it unchanged. Put this in the same test file under tag `[material_settings]` (or `[config]` to match the sibling — follow the sibling's tag).

- [ ] **Step 2: Run to confirm failure**
```bash
make test; echo "make exit: $?"
./build/bin/helix-tests "[material_settings]"
```
Expected: FAIL/compile-error — `set_preset_filament`/`get_preset_filament`/`PresetFilament` and the migration don't exist. (Do NOT pipe `make` through `tail` — that masks the exit code; L092.)

- [ ] **Step 3: Add the struct + members + declarations to the header**

In `include/material_settings_manager.h`, add (`#include <optional>`, `#include <array>`; include or forward-declare `helix::printer::EffectiveFilament` — a forward decl in the `helix::printer` namespace avoids a heavy include). In the public section:
```cpp
    struct PresetFilament {
        std::string filament_id;
        std::string brand;
        std::string name;
        int nozzle = 0;
        int bed = 0;
        bool is_branded() const { return !filament_id.empty(); }
    };

    std::optional<PresetFilament> get_preset_filament(int index) const;
    void set_preset_filament(int index, const helix::printer::EffectiveFilament& ef);
    void clear_preset_filament(int index);
```
Parallel member next to `preset_materials_` (`.h:77`):
```cpp
    std::array<std::optional<PresetFilament>, 4> preset_filaments_{};
```

- [ ] **Step 4: Rewrite load/save for the object format + add set/get/clear**

Replace `load_presets_from_config()` (`material_settings_manager.cpp:124-149`) so it reads the **object** format, tolerating legacy bare strings, and populates BOTH `preset_materials_` (type) and `preset_filaments_` (branding):
```cpp
void MaterialSettingsManager::load_presets_from_config() {
    assign_defaults();                       // preset_materials_ = DEFAULT_PRESET_MATERIALS
    for (auto& e : preset_filaments_) e.reset();
    Config* config = Config::get_instance();
    if (!config || !config->exists("/preset_materials")) return;
    try {
        auto& arr = config->get_json("/preset_materials");
        if (!arr.is_array() || arr.size() != 4) return;   // malformed -> keep defaults
        for (int i = 0; i < 4; ++i) {
            const auto& v = arr[i];
            if (v.is_string()) {                          // legacy / defensive
                std::string s = v.get<std::string>();
                if (!s.empty()) preset_materials_[i] = s;
                continue;
            }
            if (!v.is_object()) continue;
            if (v.contains("type") && v["type"].is_string()) {
                std::string t = v["type"].get<std::string>();
                if (!t.empty()) preset_materials_[i] = t;
            }
            if (v.contains("filament_id") && v["filament_id"].is_string() &&
                !v["filament_id"].get<std::string>().empty()) {
                PresetFilament pf;
                pf.filament_id = v["filament_id"].get<std::string>();
                if (v.contains("brand") && v["brand"].is_string()) pf.brand = v["brand"].get<std::string>();
                if (v.contains("name") && v["name"].is_string()) pf.name = v["name"].get<std::string>();
                if (v.contains("nozzle") && v["nozzle"].is_number_integer()) pf.nozzle = v["nozzle"].get<int>();
                if (v.contains("bed") && v["bed"].is_number_integer()) pf.bed = v["bed"].get<int>();
                preset_filaments_[i] = pf;
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("[MaterialSettingsManager] Failed to load presets: {}", e.what());
        assign_defaults();
    }
}
```
Replace `save_presets_to_config()` (`:151-162`) to write the object array:
```cpp
void MaterialSettingsManager::save_presets_to_config() {
    Config* config = Config::get_instance();
    if (!config) return;
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        nlohmann::json entry = nlohmann::json::object();
        entry["type"] = preset_materials_[i];
        if (preset_filaments_[i] && preset_filaments_[i]->is_branded()) {
            const auto& pf = *preset_filaments_[i];
            entry["filament_id"] = pf.filament_id;
            entry["brand"] = pf.brand;
            entry["name"] = pf.name;
            entry["nozzle"] = pf.nozzle;
            entry["bed"] = pf.bed;
        }
        arr.push_back(entry);
    }
    config->get_json("/preset_materials") = arr;
    config->save();
}
```
Add the new accessors/mutators:
```cpp
std::optional<MaterialSettingsManager::PresetFilament>
MaterialSettingsManager::get_preset_filament(int index) const {
    if (index < 0 || index >= 4) return std::nullopt;
    return preset_filaments_[index];
}

void MaterialSettingsManager::set_preset_filament(
    int index, const helix::printer::EffectiveFilament& ef) {
    if (index < 0 || index >= 4) return;
    PresetFilament pf;
    pf.filament_id = ef.id; pf.brand = ef.brand; pf.name = ef.name;
    pf.nozzle = ef.nozzle_recommended; pf.bed = ef.bed_temp;
    preset_filaments_[index] = pf;
    if (!ef.type.empty()) preset_materials_[index] = ef.type;  // type in lockstep
    save_presets_to_config();
}

void MaterialSettingsManager::clear_preset_filament(int index) {
    if (index < 0 || index >= 4) return;
    preset_filaments_[index].reset();
    save_presets_to_config();
}
```
In `set_preset_material()` (`:164-171`) add `preset_filaments_[index].reset();` before the existing `save_presets_to_config()` call (plain type-swap reverts to generic). In `reset_preset_materials()` (`:173-177`) clear all `preset_filaments_` before saving. (No `preset_filaments_`-specific load/save helpers are needed — everything rides in `load_presets_from_config`/`save_presets_to_config`.)

- [ ] **Step 5: Add the config migration `v18 → v19`**

In `include/config.h:58` bump `CURRENT_CONFIG_VERSION` to `19`. In `src/system/config.cpp`, add to the anon namespace (mirror `migrate_v17_to_v18` at `:774-797`):
```cpp
static void migrate_v18_to_v19(json& config) {
    // /preset_materials: 4-string array -> 4-object array {"type": <string>}.
    // Idempotent: skip elements already objects. Never overwrite branding.
    if (!config.contains("preset_materials") || !config["preset_materials"].is_array()) return;
    json& arr = config["preset_materials"];
    for (auto& el : arr) {
        if (el.is_string()) {
            json obj = json::object();
            obj["type"] = el.get<std::string>();
            el = obj;
        }
    }
    spdlog::info("[Config] Migration v19: preset_materials strings -> objects ({} entries)",
                 arr.size());
}
```
Register it in `run_versioned_migrations()` before the final `config["config_version"] = CURRENT_CONFIG_VERSION;` line (`:843`):
```cpp
    if (version < 19)
        migrate_v18_to_v19(config);
```

- [ ] **Step 6: Run tests**
```bash
make test; echo "make exit: $?"
./build/bin/helix-tests "[material_settings]"
```
Expected: all preset + migration tests PASS. Also run `[config]` to confirm no migration-chain regression: `./build/bin/helix-tests "[config]"`.

- [ ] **Step 7: Commit**
```bash
git add include/material_settings_manager.h src/system/material_settings_manager.cpp \
        include/config.h src/system/config.cpp tests/unit/test_preset_filament_persistence.cpp
git commit -m "feat(filament): branded preset persistence via /preset_materials object migration (v18->v19)"
```

---

## Task 7: Preset long-press → picker; branded heat + label

**Files:**
- Modify: `include/ui_panel_filament.h`, `src/ui/ui_panel_filament.cpp`

**Interfaces:**
- Consumes: `FilamentCatalogPickerModal` (Task 4), `MaterialSettingsManager::set_preset_filament/get_preset_filament` (Task 6).
- Produces: long-press opens the catalog picker (seeded with the button's type); selecting persists a branded preset; tapping heats to branded temps when set.

- [ ] **Step 1: Swap the picker member in the header**

In `include/ui_panel_filament.h`, replace the include + member:
```cpp
#include "ui_filament_catalog_picker.h"   // was ui_material_picker_menu.h
...
    helix::ui::FilamentCatalogPickerModal catalog_picker_;   // was MaterialPickerMenu material_picker_;
```

- [ ] **Step 2: Rewrite `handle_preset_longpress` to open the catalog picker**

Replace the body (`src/ui/ui_panel_filament.cpp:770-781`):
```cpp
void FilamentPanel::handle_preset_longpress(int slot) {
    if (slot < 0 || slot >= PRESET_COUNT || !preset_buttons_[slot]) return;
    lv_obj_t* screen = lv_obj_get_screen(preset_buttons_[slot]);
    auto token = lifetime_.token();
    catalog_picker_.show(
        screen, std::optional<std::string>(preset_materials_[slot]),
        [this, slot, token](const helix::printer::EffectiveFilament& ef) {
            if (token.expired()) return;
            token.defer("FilamentPanel::apply_preset_pick", [this, slot, ef]() {
                helix::MaterialSettingsManager::instance().set_preset_filament(slot, ef);
                reassign_preset(slot, ef.type);  // updates label/type/highlight (existing path)
            });
        });
}
```
(Note: `reassign_preset` already persists the type via `set_preset_material` and refreshes labels; `set_preset_filament` already writes the type too — the double-write is harmless and idempotent. If review prefers, fold branded persistence into `reassign_preset`; keep the split for now.)

- [ ] **Step 3: Make preset TAP heat to branded temps when set**

In `handle_preset_button(int id)` (`ui_panel_filament.cpp:717`), before pulling generic temps from `filament::find_material`, check for a branded preset:
```cpp
auto branded = helix::MaterialSettingsManager::instance().get_preset_filament(id);
int nozzle_target, bed_target;
if (branded && branded->is_branded()) {
    nozzle_target = branded->nozzle;
    bed_target = branded->bed;
} else {
    const auto* mat = filament::find_material(preset_materials_[id]);
    nozzle_target = mat ? mat->nozzle_recommended : 0;   // match existing field access
    bed_target = mat ? mat->bed_temp : 0;
}
```
Wire `nozzle_target`/`bed_target` into the existing `TemperatureController::set_target(...)` calls (replace the generic-temp reads at `:722-741`; keep the exact controller call signatures already there).

- [ ] **Step 4: Optional label — show brand on branded presets**

If the preset button label binding shows the material name, extend `reassign_preset`/label refresh so a branded slot shows brand (e.g. `"Bambu PLA"`); leave generic slots as the type. Keep temps line reflecting the branded temps. (Cosmetic — verify against the actual label subjects `filament_preset_*_name`; do the minimal change that shows branding without breaking the binding.)

- [ ] **Step 5: Build + interactive verify**
```bash
make -j 2>&1 | tail -20
./build/bin/helix-screen --test -vv
```
Manually: long-press a preset button → picker opens seeded to that type → pick a branded product → Select → button reflects the pick → tap the button → confirm it heats to the branded temps (watch the log for the target sends). Report what you saw.

- [ ] **Step 6: Commit**
```bash
git add include/ui_panel_filament.h src/ui/ui_panel_filament.cpp
git commit -m "feat(filament): preset long-press opens catalog picker; branded heat targets"
```

---

## Task 8: Retire `MaterialPickerMenu` + delete dead preset modal

**Files:**
- Delete: `include/ui_material_picker_menu.h`, `src/ui/ui_material_picker_menu.cpp`, `ui_xml/material_picker_menu.xml`, `ui_xml/filament_preset_edit_modal.xml`
- Modify: `src/xml_registration.cpp`, `tests/unit/test_filament_preset_reassign.cpp` (if it references MaterialPickerMenu)

- [ ] **Step 1: Confirm no remaining references**
```bash
grep -rn "MaterialPickerMenu\|material_picker_menu\|filament_preset_edit" src/ include/ tests/ ui_xml/ | grep -v "ui_filament_catalog_picker"
```
Expected after Task 7: only the files to delete + their registration lines (+ possibly `test_filament_preset_reassign.cpp`). If the panel or a test still references `MaterialPickerMenu`, fix that first.

- [ ] **Step 2: Delete the dead files + registrations**
```bash
git rm include/ui_material_picker_menu.h src/ui/ui_material_picker_menu.cpp \
       ui_xml/material_picker_menu.xml ui_xml/filament_preset_edit_modal.xml
```
Remove from `src/xml_registration.cpp`: the `register_xml("material_picker_menu.xml");` and `register_xml("filament_preset_edit_modal.xml");` lines (`:595` region).

- [ ] **Step 3: Fix/retarget the reassign test**

`tests/unit/test_filament_preset_reassign.cpp` and `tests/test_helpers/material_picker_test_access.h` reference the old menu. Retarget the test to drive `reassign_preset` / `set_preset_filament` directly (the reassignment logic still exists), or delete the menu-specific parts. Keep coverage of `validate_reassignment` + `reassign_preset`.

- [ ] **Step 4: Build + full test run**
```bash
make -j 2>&1 | tail -20
make test-run 2>&1 | tail -30
```
Expected: clean build, all tests pass. Run the touched tags explicitly:
```bash
./build/bin/helix-tests "[filament_catalog],[filament_picker],[material_settings],[filament_data]" 2>&1 | tail -20
```

- [ ] **Step 5: Commit**
```bash
git add -A
git commit -m "chore(filament): retire MaterialPickerMenu + delete dead preset-edit modal"
```

---

## Self-Review (run after writing; fix inline)

- **Spec coverage:** §4 picker → Tasks 2-4. §5 catalog API → Task 1. §6 AMS → Task 5. §7 preset+persistence → Tasks 6-7. §8 dead code → Task 8. §9 testing → per-task tests. ✔ All spec sections mapped.
- **Type consistency:** `EffectiveFilament` fields used (`id`, `brand`, `name`, `type`, `nozzle_min`, `nozzle_max`, `nozzle_recommended`, `bed_temp`) all exist in `include/filament_catalog.h:12-24`. `SlotInfo` fields (`material`, `brand`, `spool_name`, `nozzle_temp_min/max`, `bed_temp`) confirmed via QR-scan block. `set_preset_filament`/`get_preset_filament`/`PresetFilament` consistent across Tasks 6-7.
- **Deviations flagged:** AMS entry-point (#1) and additive-key persistence (#2) both surfaced in "Decisions needing confirmation" for review.
- **Open verify points:** icon-font glyph (L097) and heat behavior need interactive checks (Tasks 5, 7) — noted in-step, not unit-testable.

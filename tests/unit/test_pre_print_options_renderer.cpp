// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pre_print_options_renderer.h"
#include "ui_print_preparation_manager.h"

#include "../lvgl_test_fixture.h"
#include "pre_print_option.h"
#include "printer_detector.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

/// Walk a container's children top-to-bottom and return ids of switch widgets
/// found, in display order. Helper for asserting "the rows came out in the
/// expected sequence" without coupling to LVGL widget pointers.
std::vector<std::string> child_widget_classes(lv_obj_t* container) {
    std::vector<std::string> classes;
    uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        const lv_obj_class_t* cls = lv_obj_get_class(child);
        if (cls == &lv_label_class) {
            classes.emplace_back("label");
        } else {
            // Anything else (rows, switches, etc.) — track as "row" since
            // every non-label child in our renderer output is a row.
            classes.emplace_back("row");
        }
    }
    return classes;
}

/// Build an option set with options across multiple categories. (Categories
/// are sort keys only — the renderer emits a flat row list with no
/// subheaders. This helper just exercises the multi-category sort path.)
/// Mirrors the JSON shape that `parse_pre_print_option_set` accepts but
/// builds it directly to keep the test independent of printer_database.json
/// drift.
PrePrintOptionSet make_multi_category_set() {
    PrePrintOptionSet s;
    s.macro_name = "START_PRINT";

    PrePrintOption mech;
    mech.id = "bed_mesh";
    mech.category = PrePrintCategory::Mechanical;
    mech.order = 10;
    mech.default_enabled = true;
    mech.strategy_kind = PrePrintStrategyKind::MacroParam;
    mech.strategy = PrePrintStrategyMacroParam{"SKIP_BED_MESH", "0", "1", "0"};

    PrePrintOption qual;
    qual.id = "nozzle_clean";
    qual.category = PrePrintCategory::Quality;
    qual.order = 10;
    qual.default_enabled = false;
    qual.strategy_kind = PrePrintStrategyKind::MacroParam;
    qual.strategy = PrePrintStrategyMacroParam{"SKIP_NOZZLE_CLEAN", "0", "1", "0"};

    PrePrintOption mon;
    mon.id = "ai_detect";
    mon.category = PrePrintCategory::Monitoring;
    mon.order = 10;
    mon.default_enabled = false;
    mon.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    mon.strategy = PrePrintStrategyPreStartGcode{"LOAD_AI_RUN SWITCH={value}"};

    s.options = {mech, qual, mon};
    return s;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: empty option set leaves container empty",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet empty;
    renderer.populate(container, empty, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 0);
    REQUIRE(lv_obj_get_child_count(container) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrePrintOptionsRenderer: single-category set has no subheader",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.category = PrePrintCategory::Mechanical;
    opt.order = 10;
    opt.default_enabled = true;
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    opt.strategy = PrePrintStrategyMacroParam{"SKIP_LEVELING", "0", "1", "0"};
    set.options.push_back(opt);

    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 1);
    auto rendered = renderer.rendered_ids();
    REQUIRE(rendered.size() == 1);
    REQUIRE(rendered[0] == "bed_mesh");

    // Flat list: 1 row container only, no subheader.
    REQUIRE(lv_obj_get_child_count(container) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrePrintOptionsRenderer: multi-category set emits flat row list",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 3);
    REQUIRE(renderer.rendered_ids() ==
            std::vector<std::string>{"bed_mesh", "nozzle_clean", "ai_detect"});

    // 3 rows, no category subheaders — the section title comes from the
    // surrounding XML card, not the renderer.
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // Display order: row, row, row.
    auto classes = child_widget_classes(container);
    REQUIRE(classes == std::vector<std::string>{"row", "row", "row"});
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: state subjects initialized from default_enabled",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    // bed_mesh: default_enabled=true -> 1
    REQUIRE(renderer.get_state("bed_mesh") == 1);
    // nozzle_clean: default_enabled=false -> 0
    REQUIRE(renderer.get_state("nozzle_clean") == 0);
    // ai_detect: default_enabled=false -> 0
    REQUIRE(renderer.get_state("ai_detect") == 0);

    // Unknown id: returns the supplied default (0 by default).
    REQUIRE(renderer.get_state("does_not_exist") == 0);
    REQUIRE(renderer.get_state("does_not_exist", 42) == 42);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: visibility lookup hides row when subject is 0",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    // Single-option set with a paired visibility subject.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.category = PrePrintCategory::Mechanical;
    opt.order = 10;
    opt.default_enabled = true;
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    opt.strategy = PrePrintStrategyMacroParam{"SKIP_LEVELING", "0", "1", "0"};
    set.options.push_back(opt);

    lv_subject_t can_show{};
    lv_subject_init_int(&can_show, 1); // start visible

    auto vis_lookup = [&](const std::string& id) -> lv_subject_t* {
        return id == "bed_mesh" ? &can_show : nullptr;
    };

    renderer.populate(container, set, vis_lookup, nullptr);
    REQUIRE(renderer.row_count() == 1);

    lv_obj_t* row = renderer.get_row("bed_mesh");
    REQUIRE(row != nullptr);

    // Initially visible.
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Flipping the visibility subject hides the row.
    lv_subject_set_int(&can_show, 0);
    REQUIRE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // And restoring re-shows it.
    lv_subject_set_int(&can_show, 1);
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Tear down: clear the renderer FIRST so observers are uninstalled
    // before we deinit the local visibility subject (avoids dangling
    // observer pointers).
    renderer.clear();
    lv_subject_deinit(&can_show);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: set_state updates subject and persists across reads",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.get_state("nozzle_clean") == 0);
    renderer.set_state("nozzle_clean", 1);
    REQUIRE(renderer.get_state("nozzle_clean") == 1);

    // No-op for unknown id (must not crash).
    renderer.set_state("does_not_exist", 1);
    REQUIRE(renderer.get_state("does_not_exist") == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: AD5M Pro live DB entry produces one row, no subheader",
                 "[print_file_detail][pre_print_options][db]") {
    // Sanity-checks the live printer_database.json: AD5M Pro currently has a
    // single mechanical option (bed_mesh). Only one category present means
    // exactly one subheader (per category) is emitted. If the DB grows new
    // categories for this printer, this test will need adjustment.
    auto set = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == set.options.size());
    // Every option must have produced a corresponding row.
    for (const auto& opt : set.options) {
        REQUIRE(renderer.get_row(opt.id) != nullptr);
        REQUIRE(renderer.get_switch(opt.id) != nullptr);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "PrePrintOptionsRenderer: K1C live DB entry produces one row",
                 "[print_file_detail][pre_print_options][db]") {
    auto set = PrinterDetector::get_pre_print_option_set("Creality K1C");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    // K1C currently has just bed_mesh (PREPARE param + PRINT_PREPARED
    // pre-start gcode). Verify the row exists with a switch widget.
    REQUIRE(renderer.row_count() == set.options.size());
    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    REQUIRE(renderer.get_switch("bed_mesh") != nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: K2 Plus live DB renders bed_mesh and ai_detect",
                 "[print_file_detail][pre_print_options][db][ai_detect]") {
    // K2 Plus advertises bed_mesh (Mechanical) + ai_detect (Monitoring).
    // Renderer emits a flat row list with no subheaders (categories are sort
    // keys only; the section title comes from print_file_detail.xml's
    // PRINT OPTIONS card header).
    auto set = PrinterDetector::get_pre_print_option_set("Creality K2 Plus");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == set.options.size());
    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    REQUIRE(renderer.get_row("ai_detect") != nullptr);
    REQUIRE(renderer.get_switch("ai_detect") != nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "PrePrintOptionsRenderer: label_key wins over humanize_id",
                 "[print_file_detail][pre_print_options][label]") {
    // When `label_key` is present, the renderer must look it up via lv_tr
    // and never fall through to the humanize_id path. We verify by giving
    // the option an id that humanize_id WOULD garble (mixed case, no
    // underscores) — if label_key is honored, the row label is the i18n
    // value (or the key itself, since en.yml lacks this synthetic key);
    // if humanize_id ran, the label would have been title-cased from the id.
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption keyed;
    keyed.id = "weirdId123";                        // humanize_id would emit "WeirdId123"
    keyed.label_key = "pre_print_option.foo.label"; // not in en.yml; lv_tr returns it as-is
    keyed.category = PrePrintCategory::Mechanical;
    keyed.order = 10;
    keyed.default_enabled = false;
    keyed.strategy_kind = PrePrintStrategyKind::MacroParam;
    keyed.strategy = PrePrintStrategyMacroParam{"PARAM", "1", "0", "0"};
    set.options.push_back(keyed);

    PrePrintOption unkeyed;
    unkeyed.id = "ai_detect"; // humanize_id capitalizes after each separator -> "AI Detect"
    unkeyed.category = PrePrintCategory::Mechanical;
    unkeyed.order = 20;
    unkeyed.default_enabled = false;
    unkeyed.strategy_kind = PrePrintStrategyKind::MacroParam;
    unkeyed.strategy = PrePrintStrategyMacroParam{"PARAM2", "1", "0", "0"};
    set.options.push_back(unkeyed);

    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 2);

    // Find the label widget inside each row. Each row container's first
    // child is the label (it's added before the switch).
    auto label_text_for = [&](const std::string& id) -> std::string {
        lv_obj_t* row = renderer.get_row(id);
        REQUIRE(row != nullptr);
        REQUIRE(lv_obj_get_child_count(row) >= 1);
        lv_obj_t* label = lv_obj_get_child(row, 0);
        REQUIRE(lv_obj_get_class(label) == &lv_label_class);
        const char* t = lv_label_get_text(label);
        return std::string(t ? t : "");
    };

    // label_key path: lv_tr returns the key when no translation exists.
    // The renderer never invokes humanize_id, so the output is exactly
    // the key string (or its translation). Either way, it is NOT the
    // title-cased id form.
    std::string keyed_text = label_text_for("weirdId123");
    REQUIRE(keyed_text != "WeirdId123");
    REQUIRE_FALSE(keyed_text.empty());

    // No label_key: humanize_id runs, then is run through lv_tr (which
    // returns the humanized form when no translation exists). humanize_id
    // capitalizes after each separator, then a fix_acronym pass uppercases
    // "Ai" -> "AI" so the final output is "AI Detect".
    std::string unkeyed_text = label_text_for("ai_detect");
    REQUIRE(unkeyed_text == "AI Detect");
}

TEST_CASE_METHOD(LVGLTestFixture, "PrePrintOptionsRenderer: clear() drops rows and resets subjects",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    auto set = make_multi_category_set();

    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 3);

    renderer.clear();
    REQUIRE(renderer.row_count() == 0);
    REQUIRE(renderer.rendered_ids().empty());
    REQUIRE(renderer.get_row("bed_mesh") == nullptr);
}

// T4: integration — manager reads option state through a provider that
// delegates to the renderer. Mirrors the production wiring in
// PrintSelectDetailView::populate_option_rows() where the renderer
// becomes the source of truth for per-option toggle state.
TEST_CASE_METHOD(LVGLTestFixture,
                 "PrePrintOptionsRenderer: provider integration with PrintPreparationManager",
                 "[print_file_detail][pre_print_options][integration]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 3);

    PrintPreparationManager manager;
    manager.set_option_state_provider(
        [&renderer](const std::string& id) { return renderer.get_state(id, -1); });

    SECTION("Initial provider readouts match default_enabled") {
        // bed_mesh: default_enabled=true → ENABLED
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
        // nozzle_clean: default_enabled=false → DISABLED
        REQUIRE(manager.get_option_state("nozzle_clean") == PrePrintOptionState::DISABLED);
        // ai_detect: default_enabled=false → DISABLED
        REQUIRE(manager.get_option_state("ai_detect") == PrePrintOptionState::DISABLED);
    }

    SECTION("Programmatic state change is reflected in manager reads") {
        renderer.set_state("bed_mesh", 0);
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);

        renderer.set_state("ai_detect", 1);
        REQUIRE(manager.get_option_state("ai_detect") == PrePrintOptionState::ENABLED);
    }

    SECTION("Unknown id falls through to NOT_APPLICABLE") {
        // The provider returns -1 for unknown ids (renderer.get_state default),
        // and the manager has no cached options without a printer_state, so
        // the result is NOT_APPLICABLE.
        REQUIRE(manager.get_option_state("does_not_exist") == PrePrintOptionState::NOT_APPLICABLE);
    }
}

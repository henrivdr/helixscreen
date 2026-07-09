// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "page_scroll_auto_inject.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {
lv_obj_t* add_vscroll(lv_obj_t* parent, int n) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 400, 200);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < n; ++i) {
        lv_obj_t* r = lv_obj_create(c);
        lv_obj_set_size(r, lv_pct(100), 60);
    }
    return c;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject attaches only to overflowing vertical scrollables",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown(); // clean slate
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_set_size(root, 480, 320);
    lv_obj_t* overflowing = add_vscroll(root, 20); // qualifies
    lv_obj_t* fits = add_vscroll(root, 1);         // no overflow -> skip
    lv_obj_t* horizontal = add_vscroll(root, 20);
    lv_obj_set_scroll_dir(horizontal, LV_DIR_HOR); // horizontal -> skip
    lv_obj_update_layout(root);

    inj.on_root_shown(root);
    CHECK(inj.managed_count() == 1);
    CHECK(lv_obj_find_by_name(overflowing, "up") != nullptr);
    CHECK(lv_obj_find_by_name(fits, "up") == nullptr);
    CHECK(lv_obj_find_by_name(horizontal, "up") == nullptr);

    // Disabling the setting detaches everything. The toggle callback drives this
    // via on_setting_toggled() (there is no subject observer — see
    // PageScrollAutoInject::init); replicate that call here.
    dsm.set_page_scroll_buttons(false);
    inj.on_setting_toggled(false);
    process_lvgl(20); // flush async gutter deletes
    CHECK(inj.managed_count() == 0);

    inj.shutdown();
}

TEST_CASE_METHOD(LVGLUITestFixture, "AutoInject skips nested scrollables",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown();
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* outer = add_vscroll(test_screen(), 5);
    lv_obj_t* inner = add_vscroll(outer, 20); // nested; must be skipped
    (void)inner;
    lv_obj_update_layout(outer);

    inj.on_root_shown(outer);
    CHECK(inj.managed_count() == 1); // outer only
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    inj.shutdown();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "AutoInject propagates managed-ancestor claim across repeated on_root_shown",
                 "[page_scroll_buttons][ui]") {
    auto& dsm = helix::DisplaySettingsManager::instance();
    dsm.init_subjects();
    auto& inj = PageScrollAutoInject::instance();
    inj.shutdown(); // clean slate
    inj.init();
    dsm.set_page_scroll_buttons(true);

    lv_obj_t* root = test_screen();
    lv_obj_t* outer = add_vscroll(root, 5);   // A: qualifies
    lv_obj_t* inner = add_vscroll(outer, 20); // B: nested inside A; also qualifies alone
    lv_obj_update_layout(root);

    // First pass: A gets attached, B is skipped as a fresh nested descendant.
    inj.on_root_shown(root);
    process_lvgl(20);
    CHECK(inj.managed_count() == 1);
    CHECK(lv_obj_find_by_name(outer, "up") != nullptr);
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    // Second pass over the SAME persistent tree (simulates navigate-away and
    // back to a cached panel). A is already in controllers_, so the fresh-attach
    // branch is skipped for it — but the ancestor claim must still propagate to
    // B. Without Fix 1, `claimed` stays false for A's subtree walk and B wrongly
    // qualifies for its own controller, producing nested gutters.
    inj.on_root_shown(root);
    process_lvgl(20);
    CHECK(inj.managed_count() == 1); // still just A; without Fix 1 this is 2
    CHECK(lv_obj_find_by_name(outer, "up") != nullptr);
    CHECK(lv_obj_find_by_name(inner, "up") == nullptr);

    inj.shutdown();
    process_lvgl(20);
}

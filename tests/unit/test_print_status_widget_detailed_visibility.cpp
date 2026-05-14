// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_detailed_visibility.cpp
 * @brief Integration test for Detailed vs Library visibility flow.
 *
 * Validates that PrintStatusWidget::update_active_layout_mode() flips visibility
 * of the two sibling active-state containers (print_card_layout = Library,
 * print_card_printing_detailed = Detailed) based on the layout_style config and
 * the effective colspan.
 *
 * Uses a mock LVGL widget tree (same pattern as test_print_status_widget_idle_thumb.cpp)
 * rather than loading the real XML — panel_widget_print_status.xml has a wide
 * dependency surface (events, subjects, custom widgets) that an integration unit test
 * shouldn't have to recreate, and update_active_layout_mode() only reads the cached
 * widget pointers from lv_obj_find_by_name(), not the XML scope.
 */

#include "../lvgl_test_fixture.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "panel_widget_manager.h"
#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
bool s_widgets_registered = false;
}

class PrintStatusDetailedVisibilityFixture : public LVGLTestFixture {
  public:
    PrintStatusDetailedVisibilityFixture() {
        if (!s_widgets_registered) {
            PanelWidgetManager::instance().init_widget_subjects();
            s_widgets_registered = true;
        }
    }

    /// Build a minimal widget tree containing every name the widget queries in attach().
    /// Only the three visibility-related siblings (print_card_layout,
    /// print_card_printing_detailed, plus their parent print_card_printing) matter for
    /// the assertions; the rest exist so lv_obj_find_by_name() returns non-null and the
    /// widget's other attach-time logic doesn't bail out early.
    lv_obj_t* create_mock_tree(lv_obj_t* parent) {
        lv_obj_t* container = lv_obj_create(parent);

        // IDLE state subtree
        lv_obj_t* idle = lv_obj_create(container);
        lv_obj_set_name(idle, "print_card_idle");
        lv_obj_t* thumb = lv_image_create(idle);
        lv_obj_set_name(thumb, "print_card_thumb");
        lv_obj_t* idle_compact = lv_obj_create(idle);
        lv_obj_set_name(idle_compact, "print_card_idle_compact");
        lv_obj_t* idle_detailed = lv_obj_create(idle);
        lv_obj_set_name(idle_detailed, "print_card_idle_detailed");
        lv_obj_t* thumb_compact = lv_image_create(idle);
        lv_obj_set_name(thumb_compact, "print_card_thumb_compact");

        // PRINTING state subtree — parent + two sibling active bodies
        lv_obj_t* printing = lv_obj_create(container);
        lv_obj_set_name(printing, "print_card_printing");

        // Library-mode active body
        lv_obj_t* layout = lv_obj_create(printing);
        lv_obj_set_name(layout, "print_card_layout");
        lv_obj_t* thumb_wrap = lv_obj_create(layout);
        lv_obj_set_name(thumb_wrap, "print_card_thumb_wrap");
        lv_obj_t* active_thumb = lv_image_create(thumb_wrap);
        lv_obj_set_name(active_thumb, "print_card_active_thumb");
        lv_obj_t* info = lv_obj_create(layout);
        lv_obj_set_name(info, "print_card_info");
        lv_obj_t* preparing_info = lv_obj_create(layout);
        lv_obj_set_name(preparing_info, "print_card_preparing_info");

        // Detailed-mode active body (sibling of print_card_layout)
        lv_obj_t* detailed = lv_obj_create(printing);
        lv_obj_set_name(detailed, "print_card_printing_detailed");

        return container;
    }
};

// =============================================================================
// Tests: Detailed vs Library visibility flow
// =============================================================================

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "Detailed active visible when layout_style=detailed + colspan=2",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    w.on_size_changed(2, 2, 400, 400);

    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);

    lv_obj_t* detailed_active = lv_obj_find_by_name(container, "print_card_printing_detailed");
    lv_obj_t* legacy_layout   = lv_obj_find_by_name(container, "print_card_layout");
    REQUIRE(detailed_active != nullptr);
    REQUIRE(legacy_layout != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(detailed_active, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(legacy_layout, LV_OBJ_FLAG_HIDDEN));

    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "Library active visible when layout_style=library + colspan=2",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "library"}});
    w.on_size_changed(2, 2, 400, 400);

    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);

    lv_obj_t* detailed_active = lv_obj_find_by_name(container, "print_card_printing_detailed");
    lv_obj_t* legacy_layout   = lv_obj_find_by_name(container, "print_card_layout");
    REQUIRE(detailed_active != nullptr);
    REQUIRE(legacy_layout != nullptr);
    REQUIRE(lv_obj_has_flag(detailed_active, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(legacy_layout, LV_OBJ_FLAG_HIDDEN));

    w.detach();
}

TEST_CASE_METHOD(PrintStatusDetailedVisibilityFixture,
                 "Switching layout_style at runtime flips visibility (library -> detailed)",
                 "[print_status][detailed_visibility]") {
    PrintStatusWidget w;
    w.set_config(nlohmann::json{{"layout_style", "library"}});
    w.on_size_changed(2, 2, 400, 400);

    lv_obj_t* container = create_mock_tree(test_screen());
    w.attach(container, test_screen());
    process_lvgl(50);

    lv_obj_t* detailed_active = lv_obj_find_by_name(container, "print_card_printing_detailed");
    lv_obj_t* legacy_layout   = lv_obj_find_by_name(container, "print_card_layout");
    REQUIRE(lv_obj_has_flag(detailed_active, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(legacy_layout, LV_OBJ_FLAG_HIDDEN));

    // Flip to detailed — set_config re-runs update_active_layout_mode() since attached.
    w.set_config(nlohmann::json{{"layout_style", "detailed"}});
    process_lvgl(50);
    REQUIRE_FALSE(lv_obj_has_flag(detailed_active, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(legacy_layout, LV_OBJ_FLAG_HIDDEN));

    w.detach();
}

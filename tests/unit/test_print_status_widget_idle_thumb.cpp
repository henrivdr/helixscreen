// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Friend access to private static subjects (header forward-declares this class
// inside namespace helix; the definition must live in the same namespace).
namespace helix {
class PrintStatusWidgetTestAccess {
  public:
    static lv_subject_t* idle_thumb_path_subject() {
        return &PrintStatusWidget::idle_thumb_path_subject_;
    }
};
} // namespace helix

static bool s_widget_registered = false;

/// Fixture for testing PrintStatusWidget idle thumbnail behavior
class PrintStatusIdleThumbFixture : public LVGLTestFixture {
  public:
    PrintStatusIdleThumbFixture() {
        if (!s_widget_registered) {
            PanelWidgetManager::instance().init_widget_subjects();
            s_widget_registered = true;
        }
    }

    /// Create minimal mock widget tree matching the XML names
    lv_obj_t* create_mock_print_card(lv_obj_t* parent) {
        lv_obj_t* container = lv_obj_create(parent);

        // Idle state container
        lv_obj_t* idle = lv_obj_create(container);
        lv_obj_set_name(idle, "print_card_idle");

        lv_obj_t* thumb = lv_image_create(idle);
        lv_obj_set_name(thumb, "print_card_thumb");

        lv_obj_t* label = lv_label_create(idle);
        lv_obj_set_name(label, "print_card_label");
        lv_label_set_text(label, "Print Files");

        // Printing state container
        lv_obj_t* printing = lv_obj_create(container);
        lv_obj_set_name(printing, "print_card_printing");

        lv_obj_t* layout = lv_obj_create(printing);
        lv_obj_set_name(layout, "print_card_layout");

        lv_obj_t* thumb_wrap = lv_obj_create(layout);
        lv_obj_set_name(thumb_wrap, "print_card_thumb_wrap");

        lv_obj_t* active_thumb = lv_image_create(thumb_wrap);
        lv_obj_set_name(active_thumb, "print_card_active_thumb");

        lv_obj_t* info = lv_obj_create(layout);
        lv_obj_set_name(info, "print_card_info");

        return container;
    }

    /// Get the image source string from the idle thumbnail widget
    std::string get_idle_thumb_src(lv_obj_t* container) {
        auto* thumb = lv_obj_find_by_name(container, "print_card_thumb");
        if (!thumb) return "";
        auto* src = lv_image_get_src(thumb);
        if (!src) return "";
        return reinterpret_cast<const char*>(src);
    }

    static constexpr const char* BENCHY_PATH = "A:assets/images/benchy_thumbnail_white.png";
};

// =============================================================================
// Tests: Idle thumbnail falls back to benchy when no history
// =============================================================================

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: idle state shows benchy when no history available",
                 "[print_status_widget][idle_thumb]") {
    // get_print_history_manager() returns nullptr in test environment
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    // Should fall back to benchy since no history manager
    auto src = get_idle_thumb_src(container);
    REQUIRE(src == BENCHY_PATH);

    widget.detach();
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: label stays 'Print Files' in idle state",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    auto* label = lv_obj_find_by_name(container, "print_card_label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == "Print Files");

    widget.detach();
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: attach/detach lifecycle with idle thumb is clean",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    // Multiple attach/detach cycles should not crash
    widget.attach(container, test_screen());
    process_lvgl(200);
    widget.detach();

    widget.attach(container, test_screen());
    process_lvgl(200);
    widget.detach();

    // LVGL processing after detach should be safe
    process_lvgl(200);
}

TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: detach invalidates alive guard for async safety",
                 "[print_status_widget][idle_thumb]") {
    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    process_lvgl(200);

    // Verify benchy is shown (no history in test env)
    REQUIRE(get_idle_thumb_src(container) == BENCHY_PATH);

    widget.detach();

    // After detach, LVGL processing should not crash
    // (validates that alive guard prevents stale async callbacks)
    process_lvgl(200);
}

// =============================================================================
// Regression: attach() must defer the initial idle reset (AD5M SY6JLLKJ)
//
// Calling reset_print_card_to_idle() synchronously from attach() cascades:
//   lv_subject_copy_string(idle_thumb_path) → bind_src observer →
//   lv_image_set_src → update_align → lv_obj_update_layout → grid_update
// which crashed in grid_update reading half-built track data when the parent
// page-grid was still mid-construction (populate_page builds widgets one at a
// time). The fix defers the initial reset via lv_async_call. This test pins
// the deferral invariant: idle_thumb_path_subject_ must NOT be touched
// synchronously during attach().
// =============================================================================
TEST_CASE_METHOD(PrintStatusIdleThumbFixture,
                 "PrintStatusWidget: attach() defers initial idle reset (no sync subject notify)",
                 "[print_status_widget][idle_thumb][regression]") {
    auto* subj = PrintStatusWidgetTestAccess::idle_thumb_path_subject();
    REQUIRE(subj != nullptr);

    // Seed the subject buffer with a sentinel so we can detect whether
    // reset_print_card_to_idle() (which rewrites to benchy) ran before
    // attach() returned, or only after the next LVGL tick.
    static constexpr const char* SENTINEL = "A:assets/images/__test_seed__.png";
    lv_subject_copy_string(subj, SENTINEL);

    auto current_value = [&]() {
        const char* p = static_cast<const char*>(subj->value.pointer);
        return std::string(p ? p : "");
    };
    REQUIRE(current_value() == SENTINEL);

    PrintStatusWidget widget;
    lv_obj_t* container = create_mock_print_card(test_screen());

    widget.attach(container, test_screen());
    std::string after_attach = current_value();

    // Pump the queue + LVGL timers so the deferred async_call can fire.
    process_lvgl(200);
    std::string after_process = current_value();

    // Invariant: attach() must NOT synchronously call reset_print_card_to_idle.
    // If it does, the bind_src observer on idle_thumb cascades through
    // lv_image_set_src → layout → grid_update on the page grid that
    // populate_page is still building sibling widgets into (AD5M SY6JLLKJ).
    REQUIRE(after_attach == SENTINEL);

    // Sanity: after one LVGL tick the deferred reset has run and rewritten
    // the subject back to benchy (no history available in this fixture).
    REQUIRE(after_process == BENCHY_PATH);

    widget.detach();
}

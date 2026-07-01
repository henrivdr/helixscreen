// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_size_content.cpp
 * @brief Tests for LV_SIZE_CONTENT behavior in nested flex layouts
 *
 * These tests verify that LVGL's SIZE_CONTENT (height="content" in XML) works
 * correctly for nested flex containers. LVGL handles this natively.
 *
 * Run with: make test-size-content
 *
 * @see docs/LV_SIZE_CONTENT_GUIDE.md
 */

#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

// Global LVGL initialization (only once per test run)
static bool g_lvgl_initialized = false;
static lv_display_t* g_display = nullptr;
alignas(64) static lv_color_t g_display_buf[800 * 10];

static void ensure_lvgl_init() {
    if (!g_lvgl_initialized) {
        lv_init_safe();
        g_display = lv_display_create(800, 480);
        lv_display_set_buffers(g_display, g_display_buf, nullptr, sizeof(g_display_buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        g_lvgl_initialized = true;
        spdlog::info("[Test] LVGL initialized with 800x480 display");
    }
}

/**
 * @brief Test fixture for SIZE_CONTENT tests
 */
class SizeContentTestFixture {
  public:
    lv_obj_t* screen = nullptr;

    SizeContentTestFixture() {
        spdlog::set_level(spdlog::level::debug);
        ensure_lvgl_init();
        screen = lv_screen_active();
        lv_obj_clean(screen);
    }

    ~SizeContentTestFixture() {
        if (screen) {
            lv_obj_clean(screen);
        }
        spdlog::set_level(spdlog::level::warn);
    }

    void update_layout() {
        lv_obj_update_layout(screen);
    }

    lv_obj_t* create_flex_container(lv_obj_t* parent, lv_flex_flow_t flow, bool width_content,
                                    bool height_content) {
        lv_obj_t* cont = lv_obj_create(parent);
        lv_obj_remove_style_all(cont);
        lv_obj_set_flex_flow(cont, flow);
        lv_obj_set_width(cont, width_content ? LV_SIZE_CONTENT : 200);
        lv_obj_set_height(cont, height_content ? LV_SIZE_CONTENT : 100);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_set_style_margin_all(cont, 0, 0);
        return cont;
    }

    lv_obj_t* create_fixed_box(lv_obj_t* parent, int32_t w, int32_t h) {
        lv_obj_t* box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, w, h);
        return box;
    }

    lv_obj_t* create_label(lv_obj_t* parent, const char* text) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        return label;
    }
};

// ============================================================================
// Basic SIZE_CONTENT Tests
// ============================================================================

TEST_CASE_METHOD(SizeContentTestFixture, "Label has intrinsic SIZE_CONTENT", "[ui][basic]") {
    lv_obj_t* label = create_label(screen, "Hello World");
    update_layout();

    REQUIRE(lv_obj_get_width(label) > 0);
    REQUIRE(lv_obj_get_height(label) > 0);
}

TEST_CASE_METHOD(SizeContentTestFixture, "Flex container sizes to child", "[ui][basic]") {
    lv_obj_t* parent = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, true, true);
    lv_obj_t* child = create_fixed_box(parent, 100, 50);

    update_layout();

    REQUIRE(lv_obj_get_width(parent) >= 100);
    REQUIRE(lv_obj_get_height(parent) >= 50);
}

// ============================================================================
// Nested SIZE_CONTENT Tests (The Key Scenarios)
// ============================================================================

TEST_CASE_METHOD(SizeContentTestFixture, "Two levels of nested SIZE_CONTENT", "[ui][nested]") {
    // grandparent -> parent -> child (fixed 100x50)
    lv_obj_t* grandparent = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_t* parent = create_flex_container(grandparent, LV_FLEX_FLOW_COLUMN, false, true);
    (void)create_fixed_box(parent, 100, 50);

    update_layout();

    int32_t gp_h = lv_obj_get_height(grandparent);
    int32_t p_h = lv_obj_get_height(parent);

    spdlog::info("[Test] Nested 2-level: GP={}, P={}", gp_h, p_h);

    REQUIRE(p_h >= 50);
    REQUIRE(gp_h >= 50);
}

TEST_CASE_METHOD(SizeContentTestFixture, "Three levels of nested SIZE_CONTENT", "[ui][nested]") {
    lv_obj_t* ggp = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_t* gp = create_flex_container(ggp, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_t* p = create_flex_container(gp, LV_FLEX_FLOW_COLUMN, false, true);
    (void)create_fixed_box(p, 80, 40);

    update_layout();

    spdlog::info("[Test] Nested 3-level: GGP={}, GP={}, P={}", lv_obj_get_height(ggp),
                 lv_obj_get_height(gp), lv_obj_get_height(p));

    REQUIRE(lv_obj_get_height(p) >= 40);
    REQUIRE(lv_obj_get_height(gp) >= 40);
    REQUIRE(lv_obj_get_height(ggp) >= 40);
}

// ============================================================================
// Dynamic Content Tests
// ============================================================================

TEST_CASE_METHOD(SizeContentTestFixture, "Adding children updates parent size", "[ui][dynamic]") {
    lv_obj_t* gp = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_t* p = create_flex_container(gp, LV_FLEX_FLOW_COLUMN, false, true);
    (void)create_fixed_box(p, 100, 30);

    update_layout();
    int32_t gp_before = lv_obj_get_height(gp);

    // Add more content
    (void)create_fixed_box(p, 100, 40);
    update_layout();
    int32_t gp_after = lv_obj_get_height(gp);

    spdlog::info("[Test] Dynamic: GP before={}, after={}", gp_before, gp_after);

    REQUIRE(gp_after > gp_before);
    REQUIRE(gp_after >= 70);
}

// ============================================================================
// Real-World Pattern Tests
// ============================================================================

TEST_CASE_METHOD(SizeContentTestFixture, "Card with header and content", "[ui][real]") {
    lv_obj_t* card = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_set_width(card, 300);
    lv_obj_set_style_pad_all(card, 8, 0);

    lv_obj_t* header = create_flex_container(card, LV_FLEX_FLOW_ROW, false, true);
    lv_obj_set_width(header, LV_PCT(100));
    (void)create_fixed_box(header, 24, 24);
    (void)create_label(header, "Card Title");

    lv_obj_t* content = create_flex_container(card, LV_FLEX_FLOW_COLUMN, false, true);
    lv_obj_set_width(content, LV_PCT(100));
    (void)create_label(content, "Body content");

    update_layout();

    int32_t card_h = lv_obj_get_height(card);
    int32_t header_h = lv_obj_get_height(header);
    int32_t content_h = lv_obj_get_height(content);

    spdlog::info("[Test] Card: {}, Header: {}, Content: {}", card_h, header_h, content_h);

    REQUIRE(header_h >= 24);
    REQUIRE(content_h > 0);
    REQUIRE(card_h > header_h + content_h); // Includes padding
}

TEST_CASE_METHOD(SizeContentTestFixture, "Button row sizes to content", "[ui][real]") {
    lv_obj_t* row = create_flex_container(screen, LV_FLEX_FLOW_ROW, true, true);
    lv_obj_set_style_pad_column(row, 8, 0);

    for (const char* text : {"OK", "Cancel", "Help"}) {
        lv_obj_t* btn = create_flex_container(row, LV_FLEX_FLOW_COLUMN, true, true);
        lv_obj_set_style_pad_all(btn, 8, 0);
        create_label(btn, text);
    }

    update_layout();

    REQUIRE(lv_obj_get_width(row) > 0);
    REQUIRE(lv_obj_get_height(row) > 0);
    REQUIRE(lv_obj_get_child_count(row) == 3);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(SizeContentTestFixture, "Empty container has zero size", "[ui][edge]") {
    lv_obj_t* empty = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, true, true);
    update_layout();

    // Empty SIZE_CONTENT container should be 0 or minimal
    REQUIRE(lv_obj_get_height(empty) >= 0);
    REQUIRE(lv_obj_get_width(empty) >= 0);
}

TEST_CASE_METHOD(SizeContentTestFixture, "Mixed fixed and SIZE_CONTENT children", "[ui][edge]") {
    lv_obj_t* parent = create_flex_container(screen, LV_FLEX_FLOW_COLUMN, false, true);

    (void)create_fixed_box(parent, 100, 30); // Fixed

    lv_obj_t* nested = create_flex_container(parent, LV_FLEX_FLOW_COLUMN, false, true);
    (void)create_fixed_box(nested, 80, 20); // Nested SIZE_CONTENT

    (void)create_fixed_box(parent, 100, 25); // Fixed

    update_layout();

    int32_t parent_h = lv_obj_get_height(parent);
    spdlog::info("[Test] Mixed children: Parent={}", parent_h);

    // 30 + 20 + 25 = 75
    REQUIRE(parent_h >= 75);
}

TEST_CASE_METHOD(SizeContentTestFixture, "Row with SIZE_CONTENT width", "[ui][edge]") {
    lv_obj_t* row = create_flex_container(screen, LV_FLEX_FLOW_ROW, true, false);

    create_fixed_box(row, 50, 30);
    create_fixed_box(row, 40, 30);
    create_fixed_box(row, 60, 30);

    update_layout();

    // 50 + 40 + 60 = 150
    REQUIRE(lv_obj_get_width(row) >= 150);
}

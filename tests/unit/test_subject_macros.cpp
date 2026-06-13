// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_macros.cpp
 * @brief Unit tests for INIT_SUBJECT_INT/STRING macros
 *
 * These macros consolidate the 3-line subject initialization pattern:
 * 1. lv_subject_init_*(subject, value)
 * 2. subjects.register_subject(subject)
 * 3. if (register_xml) lv_xml_register_subject(NULL, "name", subject)
 */

#include "../lvgl_test_fixture.h"
#include "state/subject_macros.h"
#include "subject_managed_panel.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// INIT_SUBJECT_INT Tests
// ============================================================================

TEST_CASE("INIT_SUBJECT_INT macro", "[state][subject][macro]") {
    // Use LVGLTestFixture to ensure LVGL is initialized
    LVGLTestFixture fixture;

    SECTION("initializes subject with default value") {
        SubjectManager subjects;
        lv_subject_t test_value_;

        INIT_SUBJECT_INT(test_value, 42, subjects, false);

        int value = lv_subject_get_int(&test_value_);
        REQUIRE(value == 42);
    }

    SECTION("initializes subject with zero default") {
        SubjectManager subjects;
        lv_subject_t my_counter_;

        INIT_SUBJECT_INT(my_counter, 0, subjects, false);

        int value = lv_subject_get_int(&my_counter_);
        REQUIRE(value == 0);
    }

    SECTION("registers with subjects container") {
        SubjectManager subjects;
        lv_subject_t registered_subject_;

        REQUIRE(subjects.count() == 0);

        INIT_SUBJECT_INT(registered_subject, 0, subjects, false);

        REQUIRE(subjects.count() == 1);
    }

    SECTION("registers with XML when flag is true") {
        SubjectManager subjects;
        lv_subject_t xml_subject_;

        INIT_SUBJECT_INT(xml_subject, 123, subjects, true);

        // Verify the subject was registered with XML system
        lv_subject_t* found = lv_xml_get_subject(NULL, "xml_subject");
        REQUIRE(found == &xml_subject_);
        REQUIRE(lv_subject_get_int(found) == 123);
    }

    SECTION("skips XML registration when flag is false") {
        SubjectManager subjects;
        lv_subject_t no_xml_subject_;

        INIT_SUBJECT_INT(no_xml_subject, 456, subjects, false);

        // Subject should NOT be in XML registry
        lv_subject_t* found = lv_xml_get_subject(NULL, "no_xml_subject");
        REQUIRE(found == nullptr);

        // But the subject should still be initialized
        REQUIRE(lv_subject_get_int(&no_xml_subject_) == 456);
    }

    SECTION("works with negative values") {
        SubjectManager subjects;
        lv_subject_t negative_val_;

        INIT_SUBJECT_INT(negative_val, -100, subjects, false);

        REQUIRE(lv_subject_get_int(&negative_val_) == -100);
    }

    SECTION("multiple subjects can be registered") {
        SubjectManager subjects;
        lv_subject_t first_;
        lv_subject_t second_;
        lv_subject_t third_;

        INIT_SUBJECT_INT(first, 1, subjects, false);
        INIT_SUBJECT_INT(second, 2, subjects, false);
        INIT_SUBJECT_INT(third, 3, subjects, false);

        REQUIRE(subjects.count() == 3);
        REQUIRE(lv_subject_get_int(&first_) == 1);
        REQUIRE(lv_subject_get_int(&second_) == 2);
        REQUIRE(lv_subject_get_int(&third_) == 3);
    }
}

// ============================================================================
// INIT_SUBJECT_STRING Tests
// ============================================================================

TEST_CASE("INIT_SUBJECT_STRING macro", "[state][subject][macro]") {
    // Use LVGLTestFixture to ensure LVGL is initialized
    LVGLTestFixture fixture;

    SECTION("initializes with empty string") {
        SubjectManager subjects;
        lv_subject_t empty_str_;
        char empty_str_buf_[64];

        INIT_SUBJECT_STRING(empty_str, "", subjects, false);

        const char* value = lv_subject_get_string(&empty_str_);
        REQUIRE(value != nullptr);
        REQUIRE(std::string(value) == "");
    }

    SECTION("initializes with provided default value") {
        SubjectManager subjects;
        lv_subject_t hello_str_;
        char hello_str_buf_[64];

        INIT_SUBJECT_STRING(hello_str, "Hello, World!", subjects, false);

        const char* value = lv_subject_get_string(&hello_str_);
        REQUIRE(value != nullptr);
        REQUIRE(std::string(value) == "Hello, World!");
    }

    SECTION("registers with subjects container") {
        SubjectManager subjects;
        lv_subject_t str_subject_;
        char str_subject_buf_[64];

        REQUIRE(subjects.count() == 0);

        INIT_SUBJECT_STRING(str_subject, "test", subjects, false);

        REQUIRE(subjects.count() == 1);
    }

    SECTION("registers with XML when flag is true") {
        SubjectManager subjects;
        lv_subject_t xml_str_;
        char xml_str_buf_[64];

        INIT_SUBJECT_STRING(xml_str, "XML Value", subjects, true);

        // Verify the subject was registered with XML system
        lv_subject_t* found = lv_xml_get_subject(NULL, "xml_str");
        REQUIRE(found == &xml_str_);
        REQUIRE(std::string(lv_subject_get_string(found)) == "XML Value");
    }

    SECTION("skips XML registration when flag is false") {
        SubjectManager subjects;
        lv_subject_t no_xml_str_;
        char no_xml_str_buf_[64];

        INIT_SUBJECT_STRING(no_xml_str, "Not in XML", subjects, false);

        // Subject should NOT be in XML registry
        lv_subject_t* found = lv_xml_get_subject(NULL, "no_xml_str");
        REQUIRE(found == nullptr);

        // But the subject should still be initialized
        REQUIRE(std::string(lv_subject_get_string(&no_xml_str_)) == "Not in XML");
    }

    SECTION("handles long strings within buffer") {
        SubjectManager subjects;
        lv_subject_t long_str_;
        char long_str_buf_[128];

        std::string long_value = "This is a longer string that should fit in the buffer";
        INIT_SUBJECT_STRING(long_str, long_value.c_str(), subjects, false);

        REQUIRE(std::string(lv_subject_get_string(&long_str_)) == long_value);
    }
}

// ============================================================================
// Integration Tests - Combined Usage
// ============================================================================

TEST_CASE("INIT_SUBJECT macros work together", "[state][subject][macro][integration]") {
    LVGLTestFixture fixture;

    SubjectManager subjects;

    // Simulate a typical state class with multiple subjects
    lv_subject_t temp_value_;
    lv_subject_t target_value_;
    lv_subject_t status_text_;
    char status_text_buf_[64];

    // Initialize all subjects
    INIT_SUBJECT_INT(temp_value, 2500, subjects, true);   // 250.0 degrees in decidegrees
    INIT_SUBJECT_INT(target_value, 2100, subjects, true); // 210.0 degrees target
    INIT_SUBJECT_STRING(status_text, "Heating...", subjects, true);

    // Verify all registered
    REQUIRE(subjects.count() == 3);

    // Verify XML registration
    REQUIRE(lv_xml_get_subject(NULL, "temp_value") == &temp_value_);
    REQUIRE(lv_xml_get_subject(NULL, "target_value") == &target_value_);
    REQUIRE(lv_xml_get_subject(NULL, "status_text") == &status_text_);

    // Verify values
    REQUIRE(lv_subject_get_int(&temp_value_) == 2500);
    REQUIRE(lv_subject_get_int(&target_value_) == 2100);
    REQUIRE(std::string(lv_subject_get_string(&status_text_)) == "Heating...");
}

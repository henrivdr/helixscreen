// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_fixtures.h"

#include "ui_button.h"
#include "ui_card.h"
#include "ui_dialog.h"
#include "ui_icon.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_text.h"
#include "ui_text_input.h"

#include "spdlog/spdlog.h"

// Forward declaration — defined in favorite_macro_widget.cpp, not exported in any header.
namespace helix {
void register_favorite_macro_widgets();
} // namespace helix

using namespace helix;

// ============================================================================
// MoonrakerTestFixture Implementation
// ============================================================================

MoonrakerTestFixture::MoonrakerTestFixture() {
    // Initialize printer state with subjects (skip XML registration for tests)
    m_state.init_subjects(false);

    // Create disconnected client - validation happens before network I/O
    m_client = std::make_unique<MoonrakerClient>();

    // Create API with client and state
    m_api = std::make_unique<MoonrakerAPI>(*m_client, m_state);

    spdlog::debug("[MoonrakerTestFixture] Initialized with disconnected client");
}

MoonrakerTestFixture::~MoonrakerTestFixture() {
    // Ensure API is destroyed before client (API holds reference to client)
    m_api.reset();
    m_client.reset();
    spdlog::debug("[MoonrakerTestFixture] Cleaned up");
}

// ============================================================================
// UITestFixture Implementation
// ============================================================================

UITestFixture::UITestFixture() {
    // Initialize UITest virtual input device
    UITest::init(test_screen());
    spdlog::debug("[UITestFixture] Initialized with virtual input device");
}

UITestFixture::~UITestFixture() {
    // Clean up virtual input device
    UITest::cleanup();
    spdlog::debug("[UITestFixture] Cleaned up virtual input device");
}

// ============================================================================
// FullMoonrakerTestFixture Implementation
// ============================================================================

FullMoonrakerTestFixture::FullMoonrakerTestFixture() {
    // Initialize UITest virtual input device
    // (MoonrakerTestFixture constructor already ran)
    UITest::init(test_screen());
    spdlog::debug("[FullMoonrakerTestFixture] Initialized with Moonraker + UITest");
}

FullMoonrakerTestFixture::~FullMoonrakerTestFixture() {
    // Clean up virtual input device
    UITest::cleanup();
    spdlog::debug("[FullMoonrakerTestFixture] Cleaned up");
}

// ============================================================================
// XMLTestFixture Implementation
// ============================================================================
//
// Per-instance PrinterState / MoonrakerClient / MoonrakerAPI. Subjects stay in
// the global LVGL XML scope: each test's init_subjects(true) overwrites the
// previous test's entries with valid pointers. The destructor tears down the
// test screen (and all its observers) BEFORE destroying the state, so no UI
// component outlives the subjects it references.
//
// One-time setup (XML widget classes, fonts, globals.xml, theme, event-cb
// no-ops) happens in setup_global_xml_registrations_once() behind a static
// guard — it's a process-wide side effect that only needs to run the first
// time a fixture is constructed.

bool XMLTestFixture::s_global_registered = false;

/**
 * No-op callback for optional event handlers in XML components.
 * When a component has an optional callback prop with default="",
 * LVGL tries to find a callback named "" which doesn't exist.
 * Registering this no-op callback silences those warnings.
 */
static void xml_test_noop_event_callback(lv_event_t* /*e*/) {
    // Intentionally empty - used for optional callbacks that weren't provided
}

XMLTestFixture::XMLTestFixture() : LVGLTestFixture() {
    // The parent constructor created a test_screen, but theme initialization
    // (inside the one-time setup) wants no screens present to avoid hanging.
    // Delete it; we recreate a fresh screen below.
    if (m_test_screen != nullptr) {
        lv_obj_delete(m_test_screen);
        m_test_screen = nullptr;
    }

    setup_global_xml_registrations_once();

    // Fresh per-instance state. init_subjects(true) registers subjects into the
    // global LVGL XML scope, overwriting any prior test's entries with valid
    // pointers for the duration of this test.
    m_state.init_subjects(true);

    m_client = std::make_unique<MoonrakerClient>();
    m_api = std::make_unique<MoonrakerAPI>(*m_client, m_state);

    // Recreate the test screen (theme is already applied from the one-time setup).
    m_test_screen = lv_obj_create(nullptr);
    lv_screen_load(m_test_screen);

    spdlog::debug("[XMLTestFixture] Initialized with per-instance state");
}

XMLTestFixture::~XMLTestFixture() {
    // ORDER MATTERS: destroy screen/components FIRST so no observers or XML
    // bindings hold pointers into m_state's subjects when we tear them down.
    //
    // We handle screen deletion here rather than letting ~LVGLTestFixture do it
    // because (a) state cleanup must come after, and (b) we want to nullify
    // m_test_screen so the base dtor's screen-delete becomes a no-op.
    if (m_test_screen != nullptr) {
        // Mirror ~LVGLTestFixture: if our screen is active, swap to a temp
        // screen before deleting to avoid "deleting active screen" warnings.
        lv_obj_t* active = lv_screen_active();
        if (active == m_test_screen) {
            lv_obj_t* temp = lv_obj_create(nullptr);
            lv_screen_load(temp);
        }
        lv_obj_delete(m_test_screen);
        m_test_screen = nullptr;
    }

    // Reverse-construction-order teardown. API holds a reference to client and
    // state; client is standalone. Deinit subjects last — m_state's implicit
    // member destructor runs after this function body completes.
    m_api.reset();
    m_client.reset();
    // TODO: PrinterState::init_subjects(true) appends a `[this]{ deinit_subjects(); }`
    // lambda to StaticSubjectRegistry that dangles after this dtor. Not called today,
    // but would segfault if someone adds a process-exit deinit_all() path. Needs an
    // unregister() API on StaticSubjectRegistry.
    m_state.deinit_subjects();

    spdlog::debug("[XMLTestFixture] Cleaned up");
}

void XMLTestFixture::setup_global_xml_registrations_once() {
    // Add new widget registrations, XML components, event-cb no-ops, or theme
    // setup calls here when introducing new test-only XML primitives. Everything
    // in this helper runs exactly once per process.
    if (s_global_registered)
        return;

    spdlog::debug("[XMLTestFixture] First-time initialization of global XML registrations");

    // 1. Register fonts (required before theme)
    AssetManager::register_all();

    // 2. Register globals.xml (required for constants - must come before theme)
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");

    // 3. Initialize theme (uses globals constants, registers responsive values)
    theme_manager_init(lv_display_get_default(), false); // light mode for tests

    // 4. Register custom widgets (must be done before loading components that use them)
    ui_icon_register_widget(); // icon component
    ui_text_init();            // text_heading, text_body, text_small, text_xs
    ui_text_input_init();      // text_input (textarea with bind_text support)
    ui_button_init();          // ui_button with bind_icon support
    ui_card_register();        // ui_card
    ui_temp_display_init();    // temp_display

    // 5. Register no-op callbacks for event handlers in XML components
    lv_xml_register_event_cb(nullptr, "", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_header_back_clicked", xml_test_noop_event_callback);
    // Nozzle temp panel callbacks
    lv_xml_register_event_cb(nullptr, "on_nozzle_preset_off_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_nozzle_preset_pla_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_nozzle_preset_petg_clicked",
                             xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_nozzle_preset_abs_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_nozzle_custom_clicked", xml_test_noop_event_callback);
    // Bed temp panel callbacks
    lv_xml_register_event_cb(nullptr, "on_bed_preset_off_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_bed_preset_pla_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_bed_preset_petg_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_bed_preset_abs_clicked", xml_test_noop_event_callback);
    lv_xml_register_event_cb(nullptr, "on_bed_custom_clicked", xml_test_noop_event_callback);

    // Register widgets needed by favorite_macro_config_modal
    ui_dialog_register();
    ui_switch_register();
    // Register favorite macro widget callbacks, subjects, and the new modal component
    helix::register_favorite_macro_widgets();
    // Register no-op for optional setting_toggle_row info button callback
    lv_xml_register_event_cb(nullptr, "on_setting_info_clicked", xml_test_noop_event_callback);
    // Register components used by the modal
    lv_xml_register_component_from_file("A:ui_xml/divider_horizontal.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_toggle_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/favorite_macro_config_modal.xml");

    s_global_registered = true;
}

bool XMLTestFixture::register_component(const char* component_name) {
    char path[256];
    snprintf(path, sizeof(path), "A:ui_xml/%s.xml", component_name);
    lv_result_t result = lv_xml_register_component_from_file(path);
    if (result != LV_RESULT_OK) {
        spdlog::warn("[XMLTestFixture] Failed to register component '{}' from {}", component_name,
                     path);
        return false;
    }
    spdlog::debug("[XMLTestFixture] Registered component '{}'", component_name);
    return true;
}

lv_obj_t* XMLTestFixture::create_component(const char* component_name) {
    return create_component(component_name, nullptr);
}

lv_obj_t* XMLTestFixture::create_component(const char* component_name, const char** attrs) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component_name, attrs));
    if (obj == nullptr) {
        spdlog::warn("[XMLTestFixture] Failed to create component '{}'", component_name);
    } else {
        spdlog::debug("[XMLTestFixture] Created component '{}'", component_name);
    }
    return obj;
}

// ============================================================================
// Stubs for application globals used by display_manager.cpp
// ============================================================================
// These stubs provide test-safe no-op implementations of app_globals functions
// that are referenced by display_manager.cpp but not needed in unit tests.

void app_request_quit() {
    // No-op for tests - display manager calls this on window close
    spdlog::debug("[TestStub] app_request_quit() called - no-op in tests");
}

// Stubs for app lifecycle notifications called from lv_sdl_window.c.
// The real implementations live in application.cpp which is excluded from the test build.
extern "C" void helix_notify_app_backgrounded() {
    spdlog::debug("[TestStub] helix_notify_app_backgrounded() called - no-op in tests");
}

extern "C" void helix_notify_app_foregrounded() {
    spdlog::debug("[TestStub] helix_notify_app_foregrounded() called - no-op in tests");
}

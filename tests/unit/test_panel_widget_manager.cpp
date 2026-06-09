// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "misc/lv_timer_private.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drain LVGL's async list (lv_async_call queue) by calling lv_timer_handler
/// repeatedly until no one-shot timer fires. lv_async_call schedules its
/// callback as a one-shot timer; lv_timer_handler dispatches it.
void process_async_calls() {
    for (int safety = 0; safety < 50; ++safety) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break;
            }
            t = next;
        }
        if (!fired)
            break;
    }
}

} // namespace

TEST_CASE("PanelWidget: supports_reuse defaults to true", "[panel_widget]") {
    struct TestWidget : PanelWidget {
        void attach(lv_obj_t*, lv_obj_t*) override {}
        void detach() override {}
        const char* id() const override {
            return "test";
        }
    };
    TestWidget w;
    REQUIRE(w.supports_reuse() == true);
}

TEST_CASE("PanelWidgetManager singleton access", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    auto& mgr2 = PanelWidgetManager::instance();
    REQUIRE(&mgr == &mgr2);
}

TEST_CASE("PanelWidgetManager shared resources", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    mgr.clear_shared_resources();

    SECTION("returns nullptr for unregistered type") {
        REQUIRE(mgr.shared_resource<int>() == nullptr);
    }

    SECTION("register and retrieve") {
        auto val = std::make_shared<int>(42);
        mgr.register_shared_resource<int>(val);
        REQUIRE(mgr.shared_resource<int>() != nullptr);
        REQUIRE(*mgr.shared_resource<int>() == 42);
    }

    SECTION("clear removes all resources") {
        auto val = std::make_shared<int>(99);
        mgr.register_shared_resource<int>(val);
        mgr.clear_shared_resources();
        REQUIRE(mgr.shared_resource<int>() == nullptr);
    }

    SECTION("multiple types coexist") {
        auto i = std::make_shared<int>(10);
        auto s = std::make_shared<std::string>("hello");
        mgr.register_shared_resource<int>(i);
        mgr.register_shared_resource<std::string>(s);
        REQUIRE(*mgr.shared_resource<int>() == 10);
        REQUIRE(*mgr.shared_resource<std::string>() == "hello");
        mgr.clear_shared_resources();
    }
}

TEST_CASE("PanelWidgetManager config change callbacks", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();

    SECTION("callback is invoked on notify") {
        bool called = false;
        mgr.register_rebuild_callback("test_panel", [&called]() { called = true; });
        mgr.notify_config_changed("test_panel");
        REQUIRE(called);
        mgr.unregister_rebuild_callback("test_panel");
    }

    SECTION("notify for nonexistent panel does not crash") {
        mgr.notify_config_changed("nonexistent");
    }

    SECTION("unregister removes callback") {
        int count = 0;
        mgr.register_rebuild_callback("counting", [&count]() { count++; });
        mgr.notify_config_changed("counting");
        REQUIRE(count == 1);
        mgr.unregister_rebuild_callback("counting");
        mgr.notify_config_changed("counting");
        REQUIRE(count == 1);
    }
}

TEST_CASE("PanelWidgetManager populate with null container", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    auto widgets = mgr.populate_widgets("home", nullptr);
    REQUIRE(widgets.empty());
}

TEST_CASE("Widget factories are self-registered", "[panel_widget][self_registration]") {
    lv_init_safe(); // Widget registration requires LVGL for XML event callbacks
    helix::init_widget_registrations();

    const char* expected[] = {"temperature", "temp_stack", "led",      "power_device",
                              "network",     "thermistor", "fan_stack"};
    for (const auto* id : expected) {
        INFO("Checking widget factory: " << id);
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        REQUIRE(def->factory != nullptr);
    }
}

TEST_CASE("PanelWidgetManager raw pointer shared resources", "[panel_widget][manager]") {
    auto& mgr = PanelWidgetManager::instance();
    mgr.clear_shared_resources();

    int stack_val = 77;
    mgr.register_shared_resource<int>(&stack_val);
    REQUIRE(mgr.shared_resource<int>() != nullptr);
    REQUIRE(*mgr.shared_resource<int>() == 77);
    mgr.clear_shared_resources();
}

// Regression test for AD5X bundles XG9QJ3V9 / PFEHDEXF (v0.99.49):
// SIGBUS in unsubscribe_on_delete_cb -> lv_obj_remove_event_cb_with_user_data
// during early startup, after a burst of 8 hardware-gate subjects fired in
// ~150 ms. Each firing triggered populate_page synchronously in the same
// UpdateQueue tick; the resulting backlog of N×children async deletes
// corrupted LVGL's event list (L081 family).
//
// Fix: setup_gate_observers now coalesces multiple firings within a tick
// into a single async-deferred rebuild via lv_async_call. This test
// verifies the coalescing invariant: regardless of how many gate observers
// fire in one drain, rebuild_cb runs at most once.
TEST_CASE("PanelWidgetManager coalesces multiple gate firings into one rebuild",
          "[panel_widget][manager][regression][L081]") {
    lv_init_safe();
    helix::init_widget_registrations();

    auto& mgr = PanelWidgetManager::instance();

    // Register klippy_state (always observed by setup_gate_observers) plus
    // the hardware_gate_subject for every registered widget def. Any subject
    // that doesn't already exist gets created here so the observer chain has
    // something to attach to. nullptr scope = global.
    static lv_subject_t klippy_state_subj;
    if (!lv_xml_get_subject(nullptr, "klippy_state")) {
        lv_subject_init_int(&klippy_state_subj, 0);
        lv_xml_register_subject(nullptr, "klippy_state", &klippy_state_subj);
    }

    // Static so registrations persist across SECTION re-entries; otherwise the
    // second SECTION sees the global registrations from the first but its
    // local vector is empty and we can't iterate to set values.
    static std::vector<std::pair<std::string, lv_subject_t*>> all_gate_subjs = []() {
        std::vector<std::pair<std::string, lv_subject_t*>> out;
        for (const auto& def : helix::get_all_widget_defs()) {
            if (!def.hardware_gate_subject)
                continue;
            const char* name = def.hardware_gate_subject;
            // De-dup
            bool dup = false;
            for (const auto& kv : out) {
                if (kv.first == name) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            if (auto* existing = lv_xml_get_subject(nullptr, name)) {
                out.emplace_back(name, existing);
            } else {
                auto* subj = new lv_subject_t;
                lv_subject_init_int(subj, 0);
                lv_xml_register_subject(nullptr, name, subj);
                out.emplace_back(name, subj);
            }
        }
        return out;
    }();
    REQUIRE(all_gate_subjs.size() >= 2); // need at least 2 to test coalescing

    int rebuild_count = 0;
    mgr.setup_gate_observers("test_panel", [&rebuild_count]() { ++rebuild_count; });

    auto& q = helix::ui::UpdateQueue::instance();

    SECTION("burst of N firings produces 1 rebuild") {
        // Set every gate subject to a new value. Each set fires the observer's
        // queue_update; combined with the immediate-fire from registration,
        // we get 2N callbacks queued. Without coalescing, each would fire its
        // own rebuild_cb — causing the L081 backlog corruption seen on AD5X.
        if (auto* ks = lv_xml_get_subject(nullptr, "klippy_state"))
            lv_subject_set_int(ks, 1);
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 1);
        }

        // Drain the UpdateQueue — delivers all observer callbacks.
        // Each callback either schedules a new lv_async_call (the first one
        // in the tick) or coalesces (the rest).
        q.drain();

        // No rebuild has run yet — it's queued via lv_async_call.
        REQUIRE(rebuild_count == 0);

        // Run LVGL's async list. Should fire exactly one rebuild.
        process_async_calls();
        REQUIRE(rebuild_count == 1);
    }

    SECTION("late-arriving gate after rebuild starts queues another rebuild") {
        // First burst → 1 rebuild
        if (auto* ks = lv_xml_get_subject(nullptr, "klippy_state"))
            lv_subject_set_int(ks, 1);
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 1);
        }
        q.drain();
        process_async_calls();
        REQUIRE(rebuild_count == 1);

        // Second burst (different values) after first rebuild completed →
        // pending flag was cleared, so this queues another rebuild.
        for (auto& [name, subj] : all_gate_subjs) {
            lv_subject_set_int(subj, 2);
        }
        q.drain();
        process_async_calls();
        REQUIRE(rebuild_count == 2);
    }

    // Cleanup observers so the test fixture's reset_all() doesn't see stale
    // state. setup_gate_observers stores the new vector under panel_id; the
    // destructor of ObserverGuard will call lv_observer_remove which is safe
    // because the subjects are still alive at this point.
    mgr.clear_gate_observers("test_panel");
}

namespace {

// Spy PanelWidget used by the grid-build-race regression test. Records the
// layout that the page container had at the moment its attach() ran, so the
// test can assert children are created BEFORE the container becomes a grid.
struct GridSpyWidget : helix::PanelWidget {
    static int s_layout_at_attach; // lv_obj_get_style_layout of parent at attach
    static int s_attach_count;
    static lv_obj_t* s_attached_widget;

    void attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) override {
        s_attached_widget = widget_obj;
        ++s_attach_count;
        lv_obj_t* parent = widget_obj ? lv_obj_get_parent(widget_obj) : nullptr;
        s_layout_at_attach =
            parent ? static_cast<int>(lv_obj_get_style_layout(parent, LV_PART_MAIN)) : -1;
    }
    void detach() override {}
    const char* id() const override {
        return "clock";
    }
    // Use a private, dependency-free inline XML component so we exercise the
    // real lv_xml_create() + attach() path without pulling in the clock's
    // subjects/bindings.
    std::string get_component_name() const override {
        return "test_grid_spy_widget";
    }
};

int GridSpyWidget::s_layout_at_attach = -2;
int GridSpyWidget::s_attach_count = 0;
lv_obj_t* GridSpyWidget::s_attached_widget = nullptr;

} // namespace

// Regression test for #983: SIGSEGV in LVGL grid_update() while
// PanelWidgetManager::populate_widgets() builds the home grid. The crash
// happened because the page container had LV_LAYOUT_GRID activated BEFORE its
// children (card backgrounds + widgets) were created; a widget whose attach()
// synchronously triggered lv_obj_update_layout (e.g. PrintStatusWidget ->
// lv_image_set_src -> update_align, see print_status_widget.cpp:331) cascaded a
// grid_update on a half-built grid. The fix defers grid-layout activation until
// all children exist, then runs one clean layout pass.
//
// Invariant captured here: at the moment any widget's attach() runs, the
// container's layout is NOT yet LV_LAYOUT_GRID; after populate_widgets()
// returns, the container's layout IS LV_LAYOUT_GRID and the child exists.
// This FAILS before the fix (grid active during build) and PASSES after.
TEST_CASE_METHOD(XMLTestFixture,
                 "PanelWidgetManager activates grid layout after children are built",
                 "[panel_widget][manager][regression]") {
    helix::init_widget_registrations();

    // Register a minimal, dependency-free XML component for the spy widget so
    // lv_xml_create() succeeds without needing the real widget's subjects.
    lv_xml_register_component_from_data(
        "test_grid_spy_widget",
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\"/></component>");

    // Override the 'clock' factory with our spy factory. 'clock' is a plain
    // non-gated widget; restored at the end of the test.
    const auto* clock_def = helix::find_widget_def("clock");
    REQUIRE(clock_def != nullptr);
    WidgetFactory original_clock_factory = clock_def->factory;
    helix::register_widget_factory("clock", [](const std::string&) -> std::unique_ptr<PanelWidget> {
        return std::make_unique<GridSpyWidget>();
    });

    GridSpyWidget::s_layout_at_attach = -2;
    GridSpyWidget::s_attach_count = 0;
    GridSpyWidget::s_attached_widget = nullptr;

    const std::string panel_id = "test_grid_race";

    // Write a 2-page config: page 0 is empty, page 1 (a secondary page, which
    // does NOT get registry-default widgets appended) holds exactly one
    // enabled spy widget with an explicit 1x1 grid position. Driving
    // populate_widgets on page 1 isolates the build to our spy widget.
    auto* cfg = Config::get_instance();
    nlohmann::json widget_cfg = {{"main_page_index", 0},
                                 {"next_page_id", 2},
                                 {"pages",
                                  {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                                   {{"id", "spy"},
                                    {"widgets",
                                     {{{"id", "clock"},
                                       {"enabled", true},
                                       {"col", 0},
                                       {"row", 0},
                                       {"colspan", 1},
                                       {"rowspan", 1}}}}}}}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, widget_cfg);

    // Force a reload + clear any cached active config / grid descriptors so the
    // populate path does a full rebuild.
    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);

    // A real on-screen container with a definite size so cell math is sane.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    process_lvgl(10);

    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) != LV_LAYOUT_GRID);

    auto widgets = mgr.populate_widgets(panel_id, container, /*page_index=*/1);

    // The spy widget's attach() must have run, and observed a non-grid layout.
    REQUIRE(GridSpyWidget::s_attach_count == 1);
    INFO("layout at attach (LV_LAYOUT_GRID=" << static_cast<int>(LV_LAYOUT_GRID)
                                             << ") was: " << GridSpyWidget::s_layout_at_attach);
    REQUIRE(GridSpyWidget::s_layout_at_attach != static_cast<int>(LV_LAYOUT_GRID));

    // After populate_widgets returns the grid IS active and the child exists.
    REQUIRE(lv_obj_get_style_layout(container, LV_PART_MAIN) == LV_LAYOUT_GRID);
    REQUIRE(lv_obj_get_child_count(container) > 0);
    REQUIRE(GridSpyWidget::s_attached_widget != nullptr);
    // The attached widget is parented into the page container.
    REQUIRE(lv_obj_get_parent(GridSpyWidget::s_attached_widget) == container);

    // Restore global registry state for subsequent tests.
    helix::register_widget_factory("clock", original_clock_factory);
    mgr.clear_panel_config(panel_id);
}

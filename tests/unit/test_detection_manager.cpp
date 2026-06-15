// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"
#include "detection_manager.h"

using namespace helix::detection;
namespace {
struct StubSource : DetectionSource {
    std::string id() const override { return "stub"; }
    bool available() const override { return avail; }
    void set_callback(Callback cb) override { saved = std::move(cb); }
    void fire(const DetectionEvent& e) { if (saved) saved(e); }
    bool avail = true;
    Callback saved;
};
}  // namespace

TEST_CASE_METHOD(HelixTestFixture,
        "DetectionManager dispatches to presenter under DeferToSource", "[detection][manager]") {
    auto& m = DetectionManager::instance();
    m.reset_for_test();
    auto stub = std::make_unique<StubSource>();
    auto* raw = stub.get();
    m.register_source(std::move(stub));
    m.set_policy("stub", DetectionPolicy::DeferToSource);
    int modal_shown = 0; DetectionEvent seen;
    m.set_presenter([&](const DetectionEvent& e, DetectionPolicy p) {
        if (p == DetectionPolicy::DeferToSource) { ++modal_shown; seen = e; }
    });
    DetectionEvent e; e.source_id = "stub"; e.kind = DetectionKind::Spaghetti;
    e.attributable = true; e.already_paused = true; e.message = "detected noodle";
    raw->fire(e);
    REQUIRE(modal_shown == 1);
    REQUIRE(seen.kind == DetectionKind::Spaghetti);
}

TEST_CASE_METHOD(HelixTestFixture,
        "DetectionManager respects Off policy", "[detection][manager]") {
    auto& m = DetectionManager::instance();
    m.reset_for_test();
    auto stub = std::make_unique<StubSource>();
    auto* raw = stub.get();
    m.register_source(std::move(stub));
    m.set_policy("stub", DetectionPolicy::Off);
    int calls = 0;
    m.set_presenter([&](const DetectionEvent&, DetectionPolicy) { ++calls; });
    DetectionEvent e; e.source_id = "stub"; e.kind = DetectionKind::Spaghetti;
    raw->fire(e);
    REQUIRE(calls == 0);
}

TEST_CASE_METHOD(HelixTestFixture,
        "DetectionManager any_available reflects sources", "[detection][manager]") {
    auto& m = DetectionManager::instance();
    m.reset_for_test();
    auto stub = std::make_unique<StubSource>();
    stub->avail = false;
    auto* raw = stub.get();
    m.register_source(std::move(stub));
    REQUIRE_FALSE(m.any_available());
    raw->avail = true;
    REQUIRE(m.any_available());
}

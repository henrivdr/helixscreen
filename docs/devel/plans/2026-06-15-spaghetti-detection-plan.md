# Spaghetti Detection (DetectionSource + U1 Stock Adapter) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface Snapmaker U1 stock-firmware spaghetti detections in HelixScreen — when the U1's on-device detector pauses a print, explain why, show the camera frame, and offer Resume / Abort / Tune — behind a pluggable `DetectionSource` interface so cloud/Paxx sources can slot in later.

**Architecture:** A `DetectionSource` interface normalizes "a detector said something" into a `DetectionEvent`. The one concrete v1 source, `U1StockSource`, observes the existing `print_state_enum` subject (as `AbortManager` does) and, on a transition to `PAUSED`, reads the already-parsed `print_stats.exception` (`code == 2` = noodle/spaghetti) to emit an attributable event. A `DetectionManager` singleton owns the source(s), applies a per-source policy (defer-to-source / notify-only), and drives the response UI (modal + toast) reusing existing job-control/abort/toast infrastructure. No ML, no new third-party deps, no threading or PrinterState parsing changes.

**Tech Stack:** C++17, LVGL 9.5, libhv (`hv::json`), Catch2 (amalgamated), HelixScreen subject/observer/AsyncLifetimeGuard patterns.

---

## Refinements from on-device + anchor verification (read before starting)

Confirmed against the project U1 (192.168.30.103, stock firmware) and the codebase on 2026-06-15:

- `print_stats.exception` is **already parsed** into plain members with getters `get_print_exception_id()/get_print_exception_code()/get_print_exception_message()` (`include/printer_print_state.h:251-263`, `src/printer/printer_print_state.cpp:343-374`, members `:553-555`). **Do not re-add parsing.**
- Stock `defect_detection.py` codes: `0` default/none, `1` dirty-bed, **`2` noodle/spaghetti**, `3` residue, `4` dirty-nozzle. On confirmation it fires `print_stats:update_exception_info(... code, level=2)` then `pause_resume.send_pause_command()` + `PAUSE`. The header notes the `exception.code` field is **shared** with other pause subsystems (runout uses `code 0`), so **`code == 2` is the only safe v1 discriminator** — runout/dirty-bed overlap is avoided by keying strictly on `2`.
- The exception member **persists across partial `notify_status_update` frames** (absent key → unchanged), so reading it on the `PAUSED` edge is race-free regardless of how Moonraker batches the pushes.
- **Backstop pause is dropped for v1.** With this signal, detection *is* the pause (`already_paused` is always true when we fire). A pre-pause signal would require the deferred MQTT/`detect-http` path. Policy reduces to defer-to-source (surface a modal) vs notify-only (toast).
- `print_state_enum` is set on the main thread (via the UpdateQueue path), so the source uses `observe_int_immediate` (synchronous, like `AbortManager`) — no extra marshaling, and tests need no queue draining.

These refine `docs/devel/plans/2026-06-15-spaghetti-detection-source.md`; that spec stays the design-intent record.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/detection_source.h` (create) | `DetectionKind` enum, `DetectionEvent` struct, `DetectionSource` abstract interface. Header-only, no LVGL deps. |
| `include/u1_stock_detection_source.h` (create) | `U1StockSource` — observes `print_state_enum`, reads exception getters, emits events. |
| `src/printer/u1_stock_detection_source.cpp` (create) | `U1StockSource` impl. |
| `include/detection_manager.h` (create) | `DetectionManager` singleton — source registry, policy, drives UI. |
| `src/printer/detection_manager.cpp` (create) | `DetectionManager` impl + `printer.objects.list` capability probe. |
| `include/settings_manager.h` (modify) | Add detection enabled + policy getters/setters/subjects. |
| `src/system/settings_manager.cpp` (modify) | Implement the new settings. |
| `include/ui_spaghetti_detection_modal.h` (create) | `SpaghettiDetectionModal : Modal`. |
| `src/ui/modals/ui_spaghetti_detection_modal.cpp` (create) | Modal impl (frame + message + Resume/Abort/Tune). |
| `ui_xml/components/spaghetti_detection_modal.xml` (create) | Modal layout. |
| `src/xml_registration.cpp` (modify) | Register the new XML component + event callbacks. |
| `src/app_globals.cpp` / app init (modify) | Construct + `init()` `DetectionManager`, register `U1StockSource`. |
| `tests/unit/test_detection_source.cpp` (create) | Interface/event + `U1StockSource` mapping tests. |
| `tests/unit/test_detection_manager.cpp` (create) | Manager policy/dispatch tests. |
| `tests/unit/test_spaghetti_detection_modal.cpp` (create) | Modal UI/binding tests. |

---

## Task 0: On-device verification gate (no code)

**Purpose:** Close the one open fact — observe a *live* non-empty `exception` payload (idle only showed `{}`) — and confirm the Tune transport before building the Tune path. The U1 is at `192.168.30.103` (root/`snapmaker`, sshpass).

- [ ] **Step 1: Capture a live detection payload.** Either induce a detection on a real print, or read what the stock UI does. Watch the field live:

```bash
sshpass -p snapmaker ssh -o StrictHostKeyChecking=no root@192.168.30.103 \
  'while :; do curl -s "http://127.0.0.1:7125/printer/objects/query?print_stats" \
     | tr "," "\n" | grep -E "state|exception|code|message"; echo ---; sleep 2; done'
```

Record the exact serialized `exception` object on a `code:2` fire and the `state` value. Expected: `"state":"paused"` with `exception` containing `"code":2` and a `"message"` like `"detected noodle"`.

- [ ] **Step 2: Confirm the Tune transport.** The config webhook is `defect_detection/config` (`noodle_enable`/`noodle_check_window`/`noodle_sensitivity`) AND there is a gcode command `DEFECT_DETECTION_CONFIG`. Verify which is reachable from a Moonraker client. Prefer the gcode path (always reachable via `printer.gcode.script`):

```bash
# Inspect the gcode command's accepted params:
sshpass -p snapmaker ssh root@192.168.30.103 \
  'grep -n "DEFECT_DETECTION_CONFIG\|get_int\|get_str\|cmd_DEFECT_DETECTION_CONFIG" \
   /home/lava/klipper/klippy/extras/defect_detection.py | head -30'
# Dry-run the gcode via Moonraker (read sensitivity only; do NOT toggle during a live print):
sshpass -p snapmaker ssh root@192.168.30.103 \
  'curl -s -X POST "http://127.0.0.1:7125/printer/gcode/script?script=DEFECT_DETECTION_CONFIG"'
```

Record the exact gcode param names (e.g. `NOODLE_ENABLE`, `NOODLE_SENSITIVITY`). If the gcode command is reachable and parameterized, Task 6 Tune uses `printer.gcode.script`; otherwise fall back to the `defect_detection/config` webhook via `send_jsonrpc`.

- [ ] **Step 3: Record findings** in a comment block at the top of `src/printer/u1_stock_detection_source.cpp` (created in Task 2) so the next engineer has the ground truth.

**No commit** (investigation only).

---

## Task 1: `DetectionSource` interface + event types

**Files:**
- Create: `include/detection_source.h`
- Test: `tests/unit/test_detection_source.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_detection_source.cpp
#include "../catch_amalgamated.hpp"
#include "detection_source.h"

using helix::detection::DetectionEvent;
using helix::detection::DetectionKind;

TEST_CASE("DetectionEvent defaults are inert", "[detection][source]") {
    DetectionEvent e;
    REQUIRE(e.kind == DetectionKind::Unknown);
    REQUIRE(e.attributable == false);
    REQUIRE(e.already_paused == false);
    REQUIRE(e.source_id.empty());
    REQUIRE_FALSE(e.confidence.has_value());
}

TEST_CASE("DetectionKind maps the stock U1 code space", "[detection][source]") {
    // code 2 == spaghetti is the only v1-surfaced kind.
    REQUIRE(helix::detection::kind_from_u1_code(2) == DetectionKind::Spaghetti);
    REQUIRE(helix::detection::kind_from_u1_code(1) == DetectionKind::DirtyBed);
    REQUIRE(helix::detection::kind_from_u1_code(3) == DetectionKind::Residue);
    REQUIRE(helix::detection::kind_from_u1_code(4) == DetectionKind::DirtyNozzle);
    REQUIRE(helix::detection::kind_from_u1_code(0) == DetectionKind::Unknown);
    REQUIRE(helix::detection::kind_from_u1_code(-1) == DetectionKind::Unknown);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test-build && ./build/bin/helix-tests "[detection][source]"`
Expected: FAIL — `detection_source.h` not found / `kind_from_u1_code` undefined.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/detection_source.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace helix::detection {

enum class DetectionKind { Spaghetti, DirtyBed, Residue, DirtyNozzle, Unknown };

/// Map a Snapmaker U1 `print_stats.exception.code` to a kind.
/// Stock defect_detection.py: 1=dirty-bed, 2=noodle/spaghetti, 3=residue,
/// 4=dirty-nozzle. 0 / -1 / anything else = not a visual defect we surface.
inline DetectionKind kind_from_u1_code(int code) {
    switch (code) {
        case 1: return DetectionKind::DirtyBed;
        case 2: return DetectionKind::Spaghetti;
        case 3: return DetectionKind::Residue;
        case 4: return DetectionKind::DirtyNozzle;
        default: return DetectionKind::Unknown;
    }
}

/// Normalized "a detector reported a defect" event.
struct DetectionEvent {
    std::string          source_id;        // e.g. "u1_stock"
    DetectionKind        kind = DetectionKind::Unknown;
    bool                 attributable = false;  // true: we KNOW the cause
    std::optional<float> confidence;            // none for U1 stock (post-threshold only)
    bool                 already_paused = false; // print_stats.state == "paused" when seen
    std::string          message;                // raw source message, e.g. "detected noodle"
};

/// A backend that can report print-failure detections. Implementations declare
/// capability via available(); the manager hides the feature where unavailable.
class DetectionSource {
public:
    using Callback = std::function<void(const DetectionEvent&)>;

    virtual ~DetectionSource() = default;
    virtual std::string id() const = 0;
    virtual bool        available() const = 0;        // gates UI visibility
    virtual bool        can_tune() const { return false; }
    virtual bool        self_pauses() const { return true; }
    /// Callback fires on the MAIN thread (already marshaled).
    virtual void        set_callback(Callback cb) = 0;
};

}  // namespace helix::detection
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test-build && ./build/bin/helix-tests "[detection][source]"`
Expected: PASS (both cases).

- [ ] **Step 5: Commit**

```bash
git add include/detection_source.h tests/unit/test_detection_source.cpp
git commit -m "feat(detection): DetectionSource interface + event types"
```

---

## Task 2: `U1StockSource` adapter

**Files:**
- Create: `include/u1_stock_detection_source.h`, `src/printer/u1_stock_detection_source.cpp`
- Test: add to `tests/unit/test_detection_source.cpp`
- Reference: `src/abort/abort_manager.cpp:307-313` (observe pattern), `include/printer_print_state.h:251-263` (getters), `include/printer_state.h` (`get_print_state_enum_subject`, `update_from_status`).

The source observes the existing `print_state_enum` subject. On a transition **into** `PAUSED`, it reads `get_print_exception_code()`; if `== 2` it emits a `Spaghetti` event with `attributable=true, already_paused=true`. Capability (`available()`) is set by `DetectionManager` after the `printer.objects.list` probe (Task 4); a `set_capable(bool)` setter makes it testable without Moonraker.

- [ ] **Step 1: Write the failing test** (append to `tests/unit/test_detection_source.cpp`)

```cpp
#include "u1_stock_detection_source.h"
#include "xml_test_fixture.h"   // owns PrinterState; provides state()
#include "printer_state.h"

using helix::detection::U1StockSource;

TEST_CASE_METHOD(XMLTestFixture,
        "U1StockSource emits Spaghetti on paused+code2", "[detection][u1][slow]") {
    U1StockSource src(&state());
    src.set_capable(true);

    helix::detection::DetectionEvent got;
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent& e) { got = e; ++fired; });
    src.start();

    // Frame 1: printing, no exception — no event.
    state().update_from_status(nlohmann::json::parse(
        R"({"print_stats":{"state":"printing","exception":{}}})"));
    REQUIRE(fired == 0);

    // Frame 2: U1 defect_detection latches noodle then pauses (one snapshot).
    state().update_from_status(nlohmann::json::parse(
        R"({"print_stats":{"state":"paused",
            "exception":{"id":532,"index":0,"code":2,"message":"detected noodle","level":2}}})"));
    REQUIRE(fired == 1);
    REQUIRE(got.kind == helix::detection::DetectionKind::Spaghetti);
    REQUIRE(got.attributable);
    REQUIRE(got.already_paused);
    REQUIRE(got.source_id == "u1_stock");
    REQUIRE(got.message == "detected noodle");
}

TEST_CASE_METHOD(XMLTestFixture,
        "U1StockSource ignores manual pause (no defect code)", "[detection][u1][slow]") {
    U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();

    state().update_from_status(nlohmann::json::parse(
        R"({"print_stats":{"state":"printing","exception":{}}})"));
    // User pause: state->paused, exception stays empty (code -1).
    state().update_from_status(nlohmann::json::parse(
        R"({"print_stats":{"state":"paused","exception":{}}})"));
    REQUIRE(fired == 0);
}

TEST_CASE_METHOD(XMLTestFixture,
        "U1StockSource fires once per pause edge", "[detection][u1][slow]") {
    U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();

    auto noodle = nlohmann::json::parse(
        R"({"print_stats":{"state":"paused","exception":{"code":2,"message":"detected noodle"}}})");
    state().update_from_status(nlohmann::json::parse(R"({"print_stats":{"state":"printing"}})"));
    state().update_from_status(noodle);
    // A redundant status frame while still paused must NOT re-fire.
    state().update_from_status(nlohmann::json::parse(R"({"print_stats":{"state":"paused"}})"));
    REQUIRE(fired == 1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test-build && ./build/bin/helix-tests "[detection][u1]"`
Expected: FAIL — `u1_stock_detection_source.h` not found.

- [ ] **Step 3: Write the header**

```cpp
// include/u1_stock_detection_source.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "detection_source.h"
#include "ui_observer_guard.h"   // ObserverGuard, observe_int_immediate
#include "printer_print_state.h" // PrintJobState

class PrinterState;

namespace helix::detection {

/// Surfaces Snapmaker U1 stock-firmware visual defect pauses (spaghetti/code 2)
/// by observing print_state_enum and reading the already-parsed
/// print_stats.exception. v1: spaghetti only. See
/// docs/devel/plans/2026-06-15-spaghetti-detection-source.md.
class U1StockSource : public DetectionSource {
public:
    explicit U1StockSource(PrinterState* state) : state_(state) {}

    std::string id() const override { return "u1_stock"; }
    bool        available() const override { return capable_; }
    bool        can_tune() const override { return true; }
    void        set_callback(Callback cb) override { cb_ = std::move(cb); }

    /// Set by DetectionManager after the printer.objects.list probe (or tests).
    void set_capable(bool v) { capable_ = v; }

    /// Begin observing print state. Idempotent.
    void start();

private:
    void on_print_state(int state_enum);

    PrinterState*               state_ = nullptr;
    Callback                    cb_;
    bool                        capable_ = false;
    int                         last_state_ = -1;   // dedup the paused edge
    helix::ui::ObserverGuard    state_observer_;
};

}  // namespace helix::detection
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/printer/u1_stock_detection_source.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On-device ground truth (Task 0, U1 192.168.30.103, stock fw, 2026-06-15):
//   print_stats.exception on a spaghetti fire = {id,index,code:2,message:"detected
//   noodle",level:2} with print_stats.state=="paused". code 2 == noodle is unique
//   to defect_detection.py; runout uses code 0, so keying on code==2 is unambiguous.
#include "u1_stock_detection_source.h"

#include "printer_state.h"
#include <spdlog/spdlog.h>

namespace helix::detection {

void U1StockSource::start() {
    if (!state_) return;
    // print_state_enum is set on the main thread via the UpdateQueue path, so an
    // immediate (synchronous) observer is safe — mirrors AbortManager.
    state_observer_ = helix::ui::observe_int_immediate<U1StockSource>(
        state_->get_print_state_enum_subject(), this,
        [](U1StockSource* self, int value) { self->on_print_state(value); });
}

void U1StockSource::on_print_state(int state_enum) {
    const int prev = last_state_;
    last_state_ = state_enum;

    // Only act on the transition INTO paused (not redundant paused frames).
    if (state_enum != static_cast<int>(PrintJobState::PAUSED)) return;
    if (prev == static_cast<int>(PrintJobState::PAUSED)) return;
    if (!cb_) return;

    const int code = state_->get_print_exception_code();
    if (kind_from_u1_code(code) != DetectionKind::Spaghetti) return;  // v1: code 2 only

    DetectionEvent e;
    e.source_id     = id();
    e.kind          = DetectionKind::Spaghetti;
    e.attributable  = true;
    e.already_paused = true;
    e.message       = state_->get_print_exception_message();
    spdlog::info("[U1StockSource] spaghetti detected (code 2): {}", e.message);
    cb_(e);
}

}  // namespace helix::detection
```

> If `PrintJobState` enum value names differ (verify in `include/printer_print_state.h`), adjust the `PAUSED` references. Confirm `get_print_state_enum_subject()` exists on `PrinterState` (it is used at `src/abort/abort_manager.cpp:307`).

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test-build && ./build/bin/helix-tests "[detection][u1]"`
Expected: PASS (all three cases).

- [ ] **Step 6: Commit**

```bash
git add include/u1_stock_detection_source.h src/printer/u1_stock_detection_source.cpp \
        tests/unit/test_detection_source.cpp
git commit -m "feat(detection): U1StockSource emits spaghetti on paused+code2"
```

---

## Task 3: `DetectionManager` (registry, policy, capability probe)

**Files:**
- Create: `include/detection_manager.h`, `src/printer/detection_manager.cpp`
- Test: `tests/unit/test_detection_manager.cpp`
- Reference: `include/abort_manager.h:93-95` (singleton + `init`), `src/abort/abort_manager.cpp:28-41`.

The manager owns sources, applies a per-source policy, and (Task 6) drives the UI. Policy enum: `DeferToSource` (default — surface a modal), `NotifyOnly` (toast), `Off`. v1 has no auto-pause/backstop (see refinements). The manager also runs the `printer.objects.list` probe to set `U1StockSource` capability. To keep dispatch testable, the manager exposes a settable "presenter" callback that the real UI wiring fills in; tests substitute a spy.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_detection_manager.cpp
#include "../catch_amalgamated.hpp"
#include "detection_manager.h"
#include "helix_test_fixture.h"

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
        "DetectionManager dispatches to presenter under DeferToSource",
        "[detection][manager]") {
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test-build && ./build/bin/helix-tests "[detection][manager]"`
Expected: FAIL — `detection_manager.h` not found.

- [ ] **Step 3: Write the header**

```cpp
// include/detection_manager.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "detection_source.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>

class MoonrakerClient;
class PrinterState;

namespace helix::detection {

enum class DetectionPolicy { Off, NotifyOnly, DeferToSource };

class DetectionManager {
public:
    static DetectionManager& instance();

    /// Wire dependencies + run the capability probe. Call once at startup.
    void init(MoonrakerClient* client, PrinterState* state);

    /// Take ownership of a source; subscribes to its callback immediately.
    void register_source(std::unique_ptr<DetectionSource> src);

    void            set_policy(const std::string& source_id, DetectionPolicy p);
    DetectionPolicy policy(const std::string& source_id) const;

    bool any_available() const;

    /// UI wiring fills this in; tests substitute a spy.
    using Presenter = std::function<void(const DetectionEvent&, DetectionPolicy)>;
    void set_presenter(Presenter p) { presenter_ = std::move(p); }

    void reset_for_test();

private:
    DetectionManager() = default;
    void on_event(const DetectionEvent& e);
    void probe_capabilities();   // printer.objects.list -> set U1 capable

    MoonrakerClient* client_ = nullptr;
    PrinterState*    state_  = nullptr;
    std::vector<std::unique_ptr<DetectionSource>> sources_;
    std::map<std::string, DetectionPolicy>        policies_;
    Presenter presenter_;
};

}  // namespace helix::detection
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/printer/detection_manager.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "detection_manager.h"

#include "i_moonraker_client.h"
#include "u1_stock_detection_source.h"
#include <spdlog/spdlog.h>

namespace helix::detection {

DetectionManager& DetectionManager::instance() {
    static DetectionManager inst;
    return inst;
}

void DetectionManager::init(MoonrakerClient* client, PrinterState* state) {
    client_ = client;
    state_  = state;
    probe_capabilities();
}

void DetectionManager::register_source(std::unique_ptr<DetectionSource> src) {
    src->set_callback([this](const DetectionEvent& e) { on_event(e); });
    if (!policies_.count(src->id())) {
        policies_[src->id()] = DetectionPolicy::DeferToSource;  // default
    }
    sources_.push_back(std::move(src));
}

void DetectionManager::set_policy(const std::string& id, DetectionPolicy p) {
    policies_[id] = p;
}

DetectionPolicy DetectionManager::policy(const std::string& id) const {
    auto it = policies_.find(id);
    return it == policies_.end() ? DetectionPolicy::DeferToSource : it->second;
}

bool DetectionManager::any_available() const {
    for (const auto& s : sources_) {
        if (s->available()) return true;
    }
    return false;
}

void DetectionManager::on_event(const DetectionEvent& e) {
    const DetectionPolicy p = policy(e.source_id);
    if (p == DetectionPolicy::Off) return;
    spdlog::info("[DetectionManager] event from {} kind={} policy={}",
                 e.source_id, static_cast<int>(e.kind), static_cast<int>(p));
    if (presenter_) presenter_(e, p);
}

void DetectionManager::probe_capabilities() {
    if (!client_) return;
    // A printer is U1-stock-detection-capable iff it registers the
    // `defect_detection` Klipper object. Probe once on init.
    client_->send_jsonrpc(
        "printer.objects.list", nlohmann::json::object(),
        [this](const nlohmann::json& resp) {
            bool has = false;
            if (auto r = resp.find("result"); r != resp.end()) {
                if (auto o = r->find("objects"); o != r->end() && o->is_array()) {
                    for (const auto& name : *o) {
                        if (name.is_string() && name.get<std::string>() == "defect_detection") {
                            has = true; break;
                        }
                    }
                }
            }
            for (auto& s : sources_) {
                if (s->id() == "u1_stock") {
                    if (auto* u1 = dynamic_cast<U1StockSource*>(s.get())) u1->set_capable(has);
                }
            }
            spdlog::info("[DetectionManager] defect_detection capable={}", has);
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[DetectionManager] objects.list probe failed: {}", err.message);
        });
}

void DetectionManager::reset_for_test() {
    sources_.clear();
    policies_.clear();
    presenter_ = nullptr;
    client_ = nullptr;
    state_  = nullptr;
}

}  // namespace helix::detection
```

> Confirm the exact `send_jsonrpc` signature/namespace and `MoonrakerError` field (`.message`) from `include/i_moonraker_client.h:59-66` (anchor-verified). Adjust the `result.objects` access if the codebase has a typed objects-list helper.

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test-build && ./build/bin/helix-tests "[detection][manager]"`
Expected: PASS (all three cases).

- [ ] **Step 6: Commit**

```bash
git add include/detection_manager.h src/printer/detection_manager.cpp \
        tests/unit/test_detection_manager.cpp
git commit -m "feat(detection): DetectionManager registry + policy + capability probe"
```

---

## Task 4: Settings (enabled + per-source policy)

**Files:**
- Modify: `include/settings_manager.h`, `src/system/settings_manager.cpp`
- Test: add to `tests/unit/` (new `test_detection_settings.cpp` or existing settings test)
- Reference: the `led_enabled` setting (`include/settings_manager.h:99-109`, `src/system/settings_manager.cpp:226-247`) and `UI_MANAGED_SUBJECT_INT` usage near `init_subjects()`.

Two settings: `detection_enabled` (bool master) and `detection_policy_u1` (int enum mapped to `DetectionPolicy`). Keep it minimal — one source today.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/unit/test_detection_settings.cpp
#include "../catch_amalgamated.hpp"
#include "settings_manager.h"
#include "helix_test_fixture.h"

TEST_CASE_METHOD(HelixTestFixture, "detection settings round-trip", "[detection][settings]") {
    auto& s = SettingsManager::instance();
    s.set_detection_enabled(true);
    REQUIRE(s.get_detection_enabled() == true);
    s.set_detection_enabled(false);
    REQUIRE(s.get_detection_enabled() == false);

    s.set_detection_policy_u1(2);  // DeferToSource
    REQUIRE(s.get_detection_policy_u1() == 2);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test-build && ./build/bin/helix-tests "[detection][settings]"`
Expected: FAIL — `set_detection_enabled` undefined.

- [ ] **Step 3: Add header declarations** (`include/settings_manager.h`, near the `led_enabled` getters)

```cpp
    bool get_detection_enabled() const;
    void set_detection_enabled(bool enabled);
    int  get_detection_policy_u1() const;       // 0=Off,1=NotifyOnly,2=DeferToSource
    void set_detection_policy_u1(int policy);
    lv_subject_t* subject_detection_enabled() { return &detection_enabled_subject_; }
```

Private members (near other subject members):

```cpp
    lv_subject_t detection_enabled_subject_{};
    lv_subject_t detection_policy_u1_subject_{};
```

- [ ] **Step 4: Implement** (`src/system/settings_manager.cpp`)

In `init_subjects()` (alongside the other `UI_MANAGED_SUBJECT_INT` calls):

```cpp
    Config* cfg = Config::get_instance();
    bool det_en = cfg->get<bool>(cfg->df() + "detection_enabled", true);
    UI_MANAGED_SUBJECT_INT(detection_enabled_subject_, det_en ? 1 : 0,
                           "settings_detection_enabled", subjects_);
    int det_pol = cfg->get<int>(cfg->df() + "detection_policy_u1", 2);  // DeferToSource
    UI_MANAGED_SUBJECT_INT(detection_policy_u1_subject_, det_pol,
                           "settings_detection_policy_u1", subjects_);
```

Methods (mirror `set_led_enabled`):

```cpp
bool SettingsManager::get_detection_enabled() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&detection_enabled_subject_)) != 0;
}
void SettingsManager::set_detection_enabled(bool enabled) {
    spdlog::info("[SettingsManager] set_detection_enabled({})", enabled);
    auto old = std::to_string(lv_subject_get_int(&detection_enabled_subject_));
    lv_subject_set_int(&detection_enabled_subject_, enabled ? 1 : 0);
    Config* cfg = Config::get_instance();
    cfg->set<bool>(cfg->df() + "detection_enabled", enabled);
    cfg->save();
    TelemetryManager::instance().notify_setting_changed(
        "detection_enabled", old, std::to_string(enabled ? 1 : 0));
}
int SettingsManager::get_detection_policy_u1() const {
    return lv_subject_get_int(const_cast<lv_subject_t*>(&detection_policy_u1_subject_));
}
void SettingsManager::set_detection_policy_u1(int policy) {
    spdlog::info("[SettingsManager] set_detection_policy_u1({})", policy);
    auto old = std::to_string(lv_subject_get_int(&detection_policy_u1_subject_));
    lv_subject_set_int(&detection_policy_u1_subject_, policy);
    Config* cfg = Config::get_instance();
    cfg->set<int>(cfg->df() + "detection_policy_u1", policy);
    cfg->save();
    TelemetryManager::instance().notify_setting_changed(
        "detection_policy_u1", old, std::to_string(policy));
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test-build && ./build/bin/helix-tests "[detection][settings]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/settings_manager.h src/system/settings_manager.cpp \
        tests/unit/test_detection_settings.cpp
git commit -m "feat(detection): persistent enabled + per-source policy settings"
```

---

## Task 5: Response modal (XML + subclass) + toast presenter

**Files:**
- Create: `ui_xml/components/spaghetti_detection_modal.xml`, `include/ui_spaghetti_detection_modal.h`, `src/ui/modals/ui_spaghetti_detection_modal.cpp`
- Modify: `src/xml_registration.cpp` (register component + callbacks)
- Test: `tests/unit/test_spaghetti_detection_modal.cpp`
- Reference: `include/ui_print_cancel_modal.h` (Modal subclass), `ui_xml/components/camera_config_modal.xml` (layout + `modal_button_row`), `src/ui/panel_widgets/camera_widget.cpp:348-356` (`lv_image_set_src` with `lv_draw_buf_t*`), `include/ui_toast_manager.h:44-56`.

- [ ] **Step 1: Write the XML component**

```xml
<!-- ui_xml/components/spaghetti_detection_modal.xml -->
<component>
  <view name="spaghetti_detection_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_pad_gap="#space_md">
    <modal_header icon_src="alert_triangle" icon_variant="error"
                  title="Print issue detected" title_tag="Print issue detected"/>
    <lv_image name="detection_preview" width="90%" height="260px" align="center"
              style_radius="#border_radius" style_clip_corner="true"/>
    <text_body name="detection_text" width="100%" align="center"/>
    <modal_button_row
        secondary_text="Abort"  secondary_callback="on_spaghetti_abort"
        primary_text="Resume"   primary_callback="on_spaghetti_resume"
        tertiary_text="Tune"    tertiary_callback="on_spaghetti_tune"/>
  </view>
</component>
```

> `icon_src="alert_triangle"` must exist in the icon set (check `codepoints.h`); substitute an existing warning icon if not. Strings here are surrounded text → translatable; do not translate product names ([L070]).

- [ ] **Step 2: Write the failing test**

```cpp
// tests/unit/test_spaghetti_detection_modal.cpp
#include "../catch_amalgamated.hpp"
#include "ui_spaghetti_detection_modal.h"
#include "lvgl_ui_test_fixture.h"

TEST_CASE_METHOD(LVGLUITestFixture,
        "SpaghettiDetectionModal shows message + invokes callbacks",
        "[detection][modal][slow]") {
    SpaghettiDetectionModal modal;
    int resumed = 0, aborted = 0;
    modal.set_on_resume([&]{ ++resumed; });
    modal.set_on_abort([&]{ ++aborted; });
    modal.set_detection("detected noodle", nullptr);

    REQUIRE(modal.show(lv_screen_active()));
    modal.invoke_resume_for_test();
    REQUIRE(resumed == 1);

    REQUIRE(modal.show(lv_screen_active()));
    modal.invoke_abort_for_test();
    REQUIRE(aborted == 1);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make test-build && ./build/bin/helix-tests "[detection][modal]"`
Expected: FAIL — header not found.

- [ ] **Step 4: Write the modal header**

```cpp
// include/ui_spaghetti_detection_modal.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ui_modal.h"
#include <functional>
#include <string>

class SpaghettiDetectionModal : public Modal {
public:
    using Action = std::function<void()>;
    const char* get_name() const override { return "Spaghetti Detection"; }
    const char* component_name() const override { return "spaghetti_detection_modal"; }

    void set_on_resume(Action a) { on_resume_ = std::move(a); }
    void set_on_abort(Action a)  { on_abort_  = std::move(a); }
    void set_on_tune(Action a)   { on_tune_   = std::move(a); }
    void set_detection(const std::string& message, lv_draw_buf_t* frame) {
        message_ = message; frame_ = frame;
    }

    // Test hooks (bypass LVGL button events):
    void invoke_resume_for_test() { if (on_resume_) on_resume_(); hide(); }
    void invoke_abort_for_test()  { if (on_abort_)  on_abort_();  hide(); }

protected:
    void on_show() override;
    void on_ok() override     { if (on_resume_) on_resume_(); hide(); }   // Resume
    void on_cancel() override  { if (on_abort_)  on_abort_();  hide(); }   // Abort
    void on_tertiary() override { if (on_tune_)  on_tune_(); }             // Tune

private:
    std::string     message_;
    lv_draw_buf_t*  frame_ = nullptr;
    Action          on_resume_, on_abort_, on_tune_;
};
```

- [ ] **Step 5: Write the modal impl**

```cpp
// src/ui/modals/ui_spaghetti_detection_modal.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_spaghetti_detection_modal.h"
#include <spdlog/spdlog.h>

void SpaghettiDetectionModal::on_show() {
    wire_ok_button("btn_primary");        // Resume
    wire_cancel_button("btn_secondary");  // Abort
    wire_tertiary_button("btn_tertiary"); // Tune

    if (lv_obj_t* txt = find_widget("detection_text")) {
        lv_label_set_text(txt, message_.c_str());
    }
    if (frame_) {
        if (lv_obj_t* img = find_widget("detection_preview")) {
            lv_image_set_src(img, frame_);  // RGB888 buffer, rotation/flip pre-applied
        }
    }
}
```

> Verify `wire_ok_button` default names map to `modal_button_row`'s primary/secondary/tertiary widgets (anchor: Modal provides `wire_*_button`; `modal_button_row` emits `btn_primary/secondary/tertiary`). `find_widget` is the inherited Modal helper.

- [ ] **Step 6: Register the component + event callbacks** (`src/xml_registration.cpp`)

Follow the existing `lv_xml_component_register_from_file(...)` + `lv_xml_register_event_cb(...)` pattern ([L014]). Register `spaghetti_detection_modal.xml` and the three callbacks (`on_spaghetti_resume/abort/tune`) that route to the active modal instance (or make the modal wire buttons in `on_show()` as above, in which case the XML `*_callback` names can be the modal's wired handlers — match whichever pattern `print_cancel_confirm_modal` uses to avoid double-wiring).

- [ ] **Step 7: Run tests to verify they pass**

Run: `make test-build && ./build/bin/helix-tests "[detection][modal]"`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add ui_xml/components/spaghetti_detection_modal.xml \
        include/ui_spaghetti_detection_modal.h \
        src/ui/modals/ui_spaghetti_detection_modal.cpp \
        src/xml_registration.cpp tests/unit/test_spaghetti_detection_modal.cpp
git commit -m "feat(detection): spaghetti response modal (resume/abort/tune)"
```

---

## Task 6: Presenter wiring + app init + Tune action

**Files:**
- Modify: app init (`src/app_globals.cpp` or the `Application`/`SubjectInitializer` startup path that constructs managers), `include/app_globals.h`
- Reference: `src/abort/abort_manager.cpp:28-41` (init order), `include/moonraker_job_api.h:62-130` (`resume_print`), `include/abort_manager.h:122` (`start_abort`).

Wire the manager into startup, register `U1StockSource`, and install the presenter that builds the modal/toast and maps its buttons to real actions.

- [ ] **Step 1: Construct + init the manager at startup** (where other managers init, after `MoonrakerClient`/`PrinterState`/`MoonrakerAPI` exist):

```cpp
auto u1 = std::make_unique<helix::detection::U1StockSource>(printer_state);
u1->start();
auto& dm = helix::detection::DetectionManager::instance();
dm.register_source(std::move(u1));
dm.init(get_moonraker_client(), printer_state);
// Apply persisted policy:
dm.set_policy("u1_stock", static_cast<helix::detection::DetectionPolicy>(
    SettingsManager::instance().get_detection_policy_u1()));
```

- [ ] **Step 2: Install the presenter**

```cpp
dm.set_presenter([](const helix::detection::DetectionEvent& e,
                    helix::detection::DetectionPolicy p) {
    using helix::detection::DetectionPolicy;
    if (!SettingsManager::instance().get_detection_enabled()) return;

    if (p == DetectionPolicy::NotifyOnly) {
        ToastManager::instance().show(ToastSeverity::WARNING,
            "Spaghetti detected — print paused", 8000);
        return;
    }
    // DeferToSource → modal. (Pointer owned for the modal's lifetime; mirror how
    // other one-shot modals manage ownership in this codebase, e.g. ModalStack.)
    auto* modal = new SpaghettiDetectionModal();
    modal->set_detection(e.message, /*frame*/ nullptr);  // frame: Step 3
    modal->set_on_resume([] {
        get_moonraker_job_api()->resume_print([]{}, [](const auto&){});
    });
    modal->set_on_abort([] { AbortManager::instance().start_abort(); });
    modal->set_on_tune([] {
        // Tune transport per Task 0 finding. Preferred: gcode command.
        get_moonraker_client()->send_jsonrpc(
            "printer.gcode.script",
            nlohmann::json{{"script", "DEFECT_DETECTION_CONFIG NOODLE_SENSITIVITY=low"}},
            [](const nlohmann::json&){}, [](const MoonrakerError&){});
    });
    modal->show(lv_screen_active());
});
```

> Replace `get_moonraker_job_api()` / `get_moonraker_client()` with the actual accessors (`app_globals.h`). Confirm the modal-ownership idiom against an existing `new`-ed modal or `ModalStack` usage so it isn't leaked; if the codebase always uses `ModalStack::push`, use that instead of raw `new`.

- [ ] **Step 3: Attach the offending camera frame (optional polish)**

There is **no public getter** for the latest frame; `CameraStream` delivers frames via callback only (`include/camera_stream.h:49-50`). If a `CameraStream`/`CameraWidget` is live when detection fires, capture the most recent delivered `lv_draw_buf_t*` into a manager-held pointer (guarded by the camera's `AsyncLifetimeGuard`) and pass it to `set_detection(..., frame)`. If no stream is active, pass `nullptr` (modal hides the image). Do **not** synchronously start a stream from the presenter. Keep this minimal; frame display is enhancement, not core.

- [ ] **Step 4: Build the program and smoke-check**

Run: `make -j`
Expected: clean build, no warnings about the new files.

- [ ] **Step 5: Commit**

```bash
git add src/app_globals.cpp include/app_globals.h
git commit -m "feat(detection): wire DetectionManager + presenter into app startup"
```

---

## Task 7: Full test sweep + on-device validation

- [ ] **Step 1: Run the full detection test set**

Run: `make test-run && ./build/bin/helix-tests "[detection]"`
Expected: all detection tests PASS. (Threaded/fixture tests are tagged `[slow]` per `mk/tests.mk` — confirm none deadlock under parallel shards.)

- [ ] **Step 2: Run the full suite to catch regressions**

Run: `make test-run`
Expected: no new failures vs. baseline.

- [ ] **Step 3: Deploy to the U1 and validate live**

```bash
make snapmaker-u1-docker && SNAPMAKER_U1_HOST=192.168.30.103 make deploy-snapmaker-u1
```

Then on a real or induced detection, verify (via logs / on-screen): the modal/toast appears with "detected noodle", Resume calls `printer.print.resume` (print resumes), Abort escalates, and the capability probe logged `defect_detection capable=true`. Confirm the feature is inert/hidden on a non-U1 target (e.g. the Pi) — `any_available()` false, no modal.

- [ ] **Step 4: Commit any fixes from live validation**, then mark the plan complete.

---

## Self-Review

**Spec coverage:**
- §4 `DetectionSource` abstraction → Task 1. ✓
- §5 `U1StockSource` (observe, map, capability) → Tasks 2 (observe/map) + 3 (capability probe). ✓
- §5 Tune via `defect_detection/config` → Task 0 (verify transport) + Task 6 Step 2 (gcode `DEFECT_DETECTION_CONFIG` preferred). ✓
- §6 policy + de-dup → Task 3 (policy); de-dup is implicit (`already_paused` always true, never self-pauses). Backstop intentionally dropped (documented in Refinements). ✓
- §7 response UI (modal, toast, frame, actions) → Tasks 5 + 6. ✓
- §8 settings (enabled, policy, spaghetti-only default) → Task 4; spaghetti-only is enforced in `U1StockSource::on_print_state` (code==2 only). ✓
- §10 threading → `observe_int_immediate` (main-thread set), no bg marshaling needed; documented. ✓
- §9 deferred cloud/Paxx → not built; interface (`attributable`, `confidence`) leaves room. ✓

**Placeholder scan:** No TBD/TODO. Two deliberate verify-on-device steps (Task 0) with concrete commands, not placeholders. Frame-display (Task 6 Step 3) is scoped as optional with a concrete fallback (`nullptr`).

**Type consistency:** `DetectionEvent`/`DetectionKind`/`DetectionPolicy`/`DetectionSource::Callback` names consistent across Tasks 1–6. `kind_from_u1_code`, `get_print_exception_code()`, `get_print_state_enum_subject()`, `send_jsonrpc`, `resume_print`, `start_abort`, `wire_*_button`, `find_widget`, `lv_image_set_src` all match anchor-verified signatures. Settings getters/setters consistent between Task 4 header and impl.

**Open risks flagged inline:** `PrintJobState::PAUSED` enum name, `modal_button_row` button-name mapping, modal ownership idiom, icon name, and Tune transport are each marked "verify against existing code/device" rather than assumed.

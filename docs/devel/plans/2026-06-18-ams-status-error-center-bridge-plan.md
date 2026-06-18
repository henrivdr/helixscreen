# AMS Status→Error-Center Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Route status-driven AMS faults (`AmsAction::ERROR`) into the same unified recovery modal the gcode `!!` path uses, via a shared presenter + a dedicated AMS trigger, with IFS and QIDI as the first consumers.

**Architecture:** Extract a shared `RecoveryModalPresenter` (single modal owner) from `GcodeErrorRouter`; the router keeps its gcode trigger and delegates presentation. Add a new `AmsBackend::current_error()` virtual and a new `AmsErrorBridge` that observes the AMS action subject and presents the backend's current error on the ERROR edge. `AmsState` stays pure.

**Tech Stack:** C++17, LVGL 9.5 subjects, Catch2, `observe_int_sync` (observer_factory), `helix::ui::ActionPromptModal`, `helix::ErrorEvent`.

## Global Constraints

- **Abstraction must not leak:** no `AmsType::`/`ErrorSource::` switch in `RecoveryModalPresenter`, `GcodeErrorRouter`, or `AmsErrorBridge`. Backends own their error semantics via `current_error()`. (Review gate: grep these files.)
- **Single modal instance:** there is exactly ONE `ActionPromptModal`, owned by `RecoveryModalPresenter`. Neither trigger may create its own.
- **`AmsState` stays pure:** it owns the action subject; it must NOT gain any dependency on modals/presenter.
- **IFS + QIDI ship blind** (no AD5X / no QIDI hardware). Recovery commands are best-effort; QIDI's BLOCKED-clearing gcode is UNKNOWN — do NOT invent one. Label commits `(ships blind)`.
- **Threading:** the AMS action subject is set on the main thread; `AmsErrorBridge` observes via `observe_int_sync` (main-thread callback). Presenting a modal there is safe. `current_error()` is read on the main thread.
- spdlog only; SPDX headers; `lv_tr()` on user-facing strings (not product names / material codes).
- Build: `make -j`; tests `make test` then `./build/bin/helix-tests "[tag]"`. Work in the worktree `/home/pbrown/Code/Printing/helixscreen/.worktrees/ams-error-center-bridge` (branch `feature/ams-error-center-bridge`).
- `current_error()` signature is fixed: `[[nodiscard]] virtual std::optional<helix::ErrorEvent> current_error() const;` (default `{ return std::nullopt; }` in base).

---

### Task 1: Extract `RecoveryModalPresenter` from `GcodeErrorRouter`

Pure refactor — no behavior change to the gcode path. Move the modal ownership + present logic into a new source-agnostic class; the router delegates.

**Files:**
- Create: `include/recovery_modal_presenter.h`, `src/ui/recovery_modal_presenter.cpp`
- Modify: `include/gcode_error_router.h`, `src/application/gcode_error_router.cpp`
- Modify: `src/application/application.cpp` (own the presenter, pass to router), `include/application.h`
- Test: `tests/unit/test_recovery_modal_presenter.cpp` (new), and existing `test_gcode_error_routing_e2e.cpp` / `test_unified_recovery_dialog.cpp` must stay green.

**Interfaces:**
- Produces:
  ```cpp
  namespace helix::ui {
  class RecoveryModalPresenter {
    public:
      explicit RecoveryModalPresenter(MoonrakerAPI* api);
      // Show the recovery modal for this event (or replace content if already
      // visible; dedups identical e.detail while visible). Falls back to
      // ui_notification_error when no api_/screen.
      void present(const helix::ErrorEvent& e);
      // Hide the modal if visible and clear shown-detail state.
      void dismiss();
      [[nodiscard]] bool is_visible() const;
    private:
      MoonrakerAPI* api_;
      std::unique_ptr<helix::ui::ActionPromptModal> modal_;
      std::string shown_detail_;
      std::vector<helix::RecoveryAction> active_actions_;
  };
  }
  ```
- Consumes: existing free functions `helix::build_recovery_prompt(const ErrorEvent&)`, `helix::decide_presentation(...)`, and `modal_title_for(const ErrorEvent&)` (currently used in `gcode_error_router.cpp`; if it's a static/file-local helper there, move it to a shared spot or duplicate the small mapping into the presenter — confirm while extracting).

- [ ] **Step 1: Write the failing test** for the presenter (`tests/unit/test_recovery_modal_presenter.cpp`):

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "RecoveryModalPresenter shows and dismisses",
                 "[error-center][presenter]") {
    helix::ui::RecoveryModalPresenter presenter(nullptr); // no api → uses fallback path; see note
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::AFC;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = "Toolhead jam";
    e.detail = "tool_end jam detected";
    e.recovery_actions = {{"Resume", "RESUME", "t::resume", "primary"}};
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    // Presenting the same detail again must not stack a second modal.
    presenter.present(e);
    process_lvgl(20);
    CHECK(presenter.is_visible());
    presenter.dismiss();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());
}
```
Note: with `api_ == nullptr`, `present_recovery_modal` currently routes to `ui_notification_error` and returns — so for a meaningful modal test, pass a mock api OR adjust the extracted code so the modal is created independent of `api_` (api_ is only needed for the gcode callback on button tap). Prefer: in the extracted presenter, create/show the modal regardless of api_, and guard only the gcode-execution callback on `api_`. Update the test accordingly (it should assert `is_visible()` with a non-null modal). Resolve this while extracting and keep the gcode path's existing api_-null fallback behavior equivalent.

- [ ] **Step 2: Run it, verify it fails** (`./build/bin/helix-tests "[error-center][presenter]"` → link/compile error: class doesn't exist).

- [ ] **Step 3: Create the presenter.** Move the body of `GcodeErrorRouter::present_recovery_modal` (`src/application/gcode_error_router.cpp:261-326`) into `RecoveryModalPresenter::present`, renaming members: `recovery_modal_`→`modal_`, `shown_recovery_detail_`→`shown_detail_`, `active_recovery_actions_`→`active_actions_`. Add `dismiss()` (hide `modal_` if visible, clear `shown_detail_`) and `is_visible()`. Keep the dedup, the gcode-callback wiring (guarded on `api_`), the `modal_title_for`/`build_recovery_prompt` logic, and the `ui_notification_error` fallback. Refactor so modal creation does NOT require `api_` (only the button-tap gcode execution does).

- [ ] **Step 4: Make the router delegate.** In `GcodeErrorRouter`: remove `recovery_modal_`, `shown_recovery_detail_`, `active_recovery_actions_` members; add a `RecoveryModalPresenter& presenter_;` reference (ctor param). Replace `present_recovery_modal(e)`'s body with `presenter_.present(e);`. Update the ctor signature `GcodeErrorRouter(MoonrakerAPI*, MoonrakerClient*, RecoveryModalPresenter&)`.

- [ ] **Step 5: Wire ownership in `Application`.** In `include/application.h` add `std::unique_ptr<helix::ui::RecoveryModalPresenter> m_recovery_presenter;` BEFORE `m_gcode_error_router`. In `application.cpp` (~3084) create the presenter first: `m_recovery_presenter = std::make_unique<helix::ui::RecoveryModalPresenter>(api);` then pass `*m_recovery_presenter` to the router ctor. In teardown (~3963/4276), reset `m_gcode_error_router` BEFORE `m_recovery_presenter`.

- [ ] **Step 6: Build + run presenter test + the existing error-routing suites.**

Run: `make -j && ./build/bin/helix-tests "[error-center][presenter]" && ./build/bin/helix-tests "[error-center]"`
Expected: presenter test PASS; the existing `[error-center]` routing/e2e/recovery-dialog tests still PASS (behavior unchanged — this proves the refactor is transparent).

- [ ] **Step 7: Commit**

```bash
git add include/recovery_modal_presenter.h src/ui/recovery_modal_presenter.cpp \
        include/gcode_error_router.h src/application/gcode_error_router.cpp \
        include/application.h src/application/application.cpp \
        tests/unit/test_recovery_modal_presenter.cpp
git commit -m "refactor(error-center): extract RecoveryModalPresenter (single modal owner) from GcodeErrorRouter"
```

---

### Task 2: `AmsBackend::current_error()` virtual + `AmsErrorBridge`

**Files:**
- Modify: `include/ams_backend.h` (add the virtual)
- Create: `include/ams_error_bridge.h`, `src/application/ams_error_bridge.cpp`
- Modify: `include/application.h`, `src/application/application.cpp` (own the bridge)
- Test: `tests/unit/test_ams_error_bridge.cpp` (new)

**Interfaces:**
- Produces on `AmsBackend`:
  ```cpp
  [[nodiscard]] virtual std::optional<helix::ErrorEvent> current_error() const { return std::nullopt; }
  ```
- Produces:
  ```cpp
  namespace helix {
  class AmsErrorBridge {
    public:
      explicit AmsErrorBridge(helix::ui::RecoveryModalPresenter& presenter);
      void start();  // installs the observer on AmsState's action subject
    private:
      void on_action_changed(int action);  // edge-detect ERROR
      helix::ui::RecoveryModalPresenter& presenter_;
      ObserverGuard action_observer_;
      int prev_action_ = -1;
      bool presented_ = false;  // we showed the modal for the current ERROR episode
  };
  }
  ```
- Consumes: `AmsState::instance().get_ams_action_subject()`, `AmsState::instance().get_backend()`, `observe_int_sync` (`observer_factory.h`), `RecoveryModalPresenter`.

- [ ] **Step 1: Write the failing test** (`tests/unit/test_ams_error_bridge.cpp`). Use a mock backend whose `current_error()` is controllable:

```cpp
namespace {
class ErrorReportingBackend : public AmsBackendMock {
  public:
    explicit ErrorReportingBackend(int slots) : AmsBackendMock(slots) {}
    void set_error(std::optional<helix::ErrorEvent> e) { err_ = std::move(e); }
    std::optional<helix::ErrorEvent> current_error() const override { return err_; }
  private:
    std::optional<helix::ErrorEvent> err_;
};
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsErrorBridge presents on ERROR edge, dismisses on exit",
                 "[error-center][ams-bridge]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto backend = std::make_unique<ErrorReportingBackend>(4);
    auto* raw = backend.get();
    ams.set_backend(std::move(backend));

    helix::ErrorEvent e;
    e.source = helix::ErrorSource::IFS; e.severity = helix::ErrorSeverity::CRITICAL;
    e.detail = "IFS unload timed out";
    e.recovery_actions = {{"Recover", "IFS_UNLOCK", "ifs::unlock", "primary"}};
    raw->set_error(e);

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::AmsErrorBridge bridge(presenter);
    bridge.start();

    // Drive action → ERROR.
    ams.set_action(AmsAction::ERROR);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK(presenter.is_visible());

    // Drive action → IDLE: bridge dismisses.
    raw->set_error(std::nullopt);
    ams.set_action(AmsAction::IDLE);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "AmsErrorBridge does nothing when current_error is null",
                 "[error-center][ams-bridge]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto backend = std::make_unique<ErrorReportingBackend>(4);  // err_ defaults to nullopt
    ams.set_backend(std::move(backend));
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::AmsErrorBridge bridge(presenter); bridge.start();
    ams.set_action(AmsAction::ERROR);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());
    ams.set_backend(nullptr);
}
```

- [ ] **Step 2: Run, verify fail** (`AmsErrorBridge` doesn't exist).

- [ ] **Step 3: Add the virtual** to `include/ams_backend.h` (near `classify_error`):
```cpp
    /// Current actionable fault for STATUS-driven backends (no `!!` line).
    /// Returns nullopt when there is no actionable error, or when a bespoke
    /// dialog owns the fault. `!!`-driven backends leave the default and use
    /// classify_error() instead.
    [[nodiscard]] virtual std::optional<helix::ErrorEvent> current_error() const {
        return std::nullopt;
    }
```
Ensure `#include "error_event.h"` is present in `ams_backend.h`.

- [ ] **Step 4: Implement `AmsErrorBridge`.** `start()` installs `action_observer_ = observe_int_sync<AmsErrorBridge>(AmsState::instance().get_ams_action_subject(), this, [](AmsErrorBridge* self, int action){ self->on_action_changed(action); });`. `on_action_changed`:
```cpp
void AmsErrorBridge::on_action_changed(int action) {
    const bool now_error = (action == static_cast<int>(AmsAction::ERROR));
    const bool was_error = (prev_action_ == static_cast<int>(AmsAction::ERROR));
    prev_action_ = action;
    if (now_error && !was_error) {
        auto* backend = AmsState::instance().get_backend();
        if (!backend) return;
        auto ev = backend->current_error();
        if (ev) { presenter_.present(*ev); presented_ = true; }
    } else if (!now_error && was_error && presented_) {
        presenter_.dismiss();
        presented_ = false;
    }
}
```

- [ ] **Step 5: Wire in `Application`.** Add `std::unique_ptr<helix::AmsErrorBridge> m_ams_error_bridge;` AFTER `m_recovery_presenter`. Create after the presenter: `m_ams_error_bridge = std::make_unique<helix::AmsErrorBridge>(*m_recovery_presenter); m_ams_error_bridge->start();`. Teardown: reset `m_ams_error_bridge` BEFORE `m_recovery_presenter` (and it's independent of the gcode router order).

- [ ] **Step 6: Build + run** `./build/bin/helix-tests "[error-center][ams-bridge]"` (PASS) and `./build/bin/helix-tests "[error-center]"` (still green).

- [ ] **Step 7: Commit**
```bash
git add include/ams_backend.h include/ams_error_bridge.h src/application/ams_error_bridge.cpp \
        include/application.h src/application/application.cpp tests/unit/test_ams_error_bridge.cpp
git commit -m "feat(error-center): AmsErrorBridge routes status-driven AmsAction::ERROR to the recovery modal"
```

---

### Task 3: IFS `current_error()` + timeout/failure → ERROR (ships blind)

**Files:** Modify `include/ams_backend_ad5x_ifs.h`, `src/printer/ams_backend_ad5x_ifs.cpp`; Test `tests/unit/test_ams_backend_ad5x_ifs.cpp`.

**Interfaces:** Produces `std::optional<helix::ErrorEvent> AmsBackendAd5xIfs::current_error() const` (override). Consumes `system_info_.action`, `system_info_.operation_detail`, `mutex_`, `helix::ErrorSource::IFS`.

- [ ] **Step 1: Write failing tests** (`[ams][ad5x_ifs][error-center]`):
  - Drive a phase (`Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true)`), then `Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120))`; assert `backend.get_system_info().action == AmsAction::ERROR` (NOT IDLE), and `backend.current_error().has_value()` with `severity==CRITICAL`, `source==IFS`, and a recovery action whose gcode is `"IFS_UNLOCK"`.
  - After `backend.recover()`, assert `get_system_info().action != AmsAction::ERROR` (cleared) and `current_error() == nullopt`.
  - Sanity: with action IDLE, `current_error() == nullopt`.

- [ ] **Step 2: Run, verify fail** (timeout currently → IDLE; `current_error` is base nullopt).

- [ ] **Step 3: Implement.** In `check_action_timeout()` replace the `action = IDLE` on expiry with `system_info_.action = AmsAction::ERROR;` and set `operation_detail` to a timed-out message (keep `end_phase_tracking_locked()` semantics but DON'T clear operation_detail — it becomes the error detail). Do the same on the `execute_gcode` `COMMAND_FAILED` path if reachable in-backend. Add the override:
```cpp
std::optional<helix::ErrorEvent> AmsBackendAd5xIfs::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.action != AmsAction::ERROR) return std::nullopt;
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::IFS;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = lv_tr("Filament System Error");
    e.detail = system_info_.operation_detail.empty()
                   ? std::string(lv_tr("Filament operation failed"))
                   : system_info_.operation_detail;
    e.sticky = true;
    e.recovery_actions = {{lv_tr("Recover"), "IFS_UNLOCK", "ifs::unlock", "primary"}};
    return e;
}
```
In `recover()` and `reset()`, after issuing `IFS_UNLOCK`, clear the error state: under `mutex_`, if `system_info_.action == AmsAction::ERROR` set it to `AmsAction::IDLE` and `operation_detail.clear()` (mirror what `cancel()` already does). Declare the override in the header.

- [ ] **Step 4: Run tests** `./build/bin/helix-tests "[ams][ad5x_ifs]"` → PASS.

- [ ] **Step 5: Commit** `feat(error-center): IFS surfaces ERROR on timeout/failure + current_error for the bridge (ships blind)`

---

### Task 4: QIDI `current_error()` from BLOCKED slot (ships blind, no recovery gcode)

**Files:** Modify `include/ams_backend_qidi.h`, `src/printer/ams_backend_qidi.cpp`; Test `tests/unit/test_ams_backend_qidi.cpp`.

**Interfaces:** Produces `std::optional<helix::ErrorEvent> AmsBackendQidi::current_error() const` (override). Consumes `slots_`, `SlotStatus::BLOCKED`, `helix::ErrorSource::QIDI`, `mutex_`.

- [ ] **Step 1: Write failing test** (`[ams][qidi_box][error-center]`): feed a `save_variables` with a negative slot value (e.g. `slot1 = -3`) via `QidiBoxTestAccess::parse_vars`, then assert `backend.current_error().has_value()` with `source==QIDI`, severity `CRITICAL`, detail mentioning the blocked lane, and `recovery_actions.empty()` (recovery is blind — no invented gcode). Sanity: all-good slots → `current_error()==nullopt`.

- [ ] **Step 2: Run, verify fail** (base nullopt).

- [ ] **Step 3: Implement:**
```cpp
std::optional<helix::ErrorEvent> AmsBackendQidi::current_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int blocked = -1;
    for (int i = 0; i < slots_.slot_count(); ++i) {
        const auto* s = slots_.get(i);
        if (s && s->info.status == SlotStatus::BLOCKED) { blocked = i; break; }
    }
    if (blocked < 0) return std::nullopt;
    helix::ErrorEvent e;
    e.source = helix::ErrorSource::QIDI;
    e.severity = helix::ErrorSeverity::CRITICAL;
    e.title = lv_tr("Filament System Error");
    e.detail = fmt::format(fmt::runtime(lv_tr("Lane {} is blocked — manual intervention required")),
                           blocked + 1);
    e.sticky = true;
    // Recovery is intentionally empty: the gcode that clears a QIDI BLOCKED slot
    // is unknown and untested (no QIDI hardware). Surface the fault; do not
    // invent a recovery command. (prestonbrown/helixscreen#1041 follow-up.)
    return e;
}
```
Confirm `present`/`build_recovery_prompt` render a CRITICAL event with EMPTY `recovery_actions` as an informational modal (no buttons beyond dismiss); if the presenter requires ≥1 action, add a single dismiss affordance rather than a recovery gcode. Declare the override in the header.

- [ ] **Step 4: Run tests** `./build/bin/helix-tests "[ams][qidi_box]"` → PASS.

- [ ] **Step 5: Commit** `feat(error-center): QIDI current_error from BLOCKED slot (recovery blind, ships blind)`

---

## Self-Review

**Spec coverage:** RecoveryModalPresenter extraction (Task 1), current_error virtual + AmsErrorBridge (Task 2), IFS timeout→ERROR + current_error + recover-clears (Task 3), QIDI current_error blind (Task 4). U1 deferred (out of scope, spec §9). Abstraction gate = review grep on the three shared files.

**Type consistency:** `current_error()` signature identical in base (Task 2), IFS (Task 3), QIDI (Task 4). `RecoveryModalPresenter::present/dismiss/is_visible` used consistently in Tasks 1–2 and the bridge. `AmsErrorBridge(RecoveryModalPresenter&)` matches the Application wiring.

**Open detail for implementer judgment (flagged, not a placeholder):** whether the presenter renders an empty-recovery CRITICAL event as a button-less modal vs needs a dismiss button (Task 4 Step 3) — resolve while implementing and note in the report.

**Ships-blind:** IFS + QIDI recovery semantics are best-effort; QIDI has NO recovery gcode by design. Commits labeled.

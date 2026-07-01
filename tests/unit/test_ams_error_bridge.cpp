// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_error_bridge.h"
#include "ams_state.h"
#include "recovery_modal_presenter.h"

#include "../catch_amalgamated.hpp"

namespace {
class ErrorReportingBackend : public AmsBackendMock {
  public:
    explicit ErrorReportingBackend(int slots) : AmsBackendMock(slots) {}
    void set_error(std::optional<helix::ErrorEvent> e) {
        err_ = std::move(e);
    }
    std::optional<helix::ErrorEvent> current_error() const override {
        return err_;
    }

  private:
    std::optional<helix::ErrorEvent> err_;
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "AmsErrorBridge presents on ERROR edge, dismisses on exit",
                 "[error-center][ams-bridge]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto backend = std::make_unique<ErrorReportingBackend>(4);
    auto* raw = backend.get();
    ams.set_backend(std::move(backend));

    helix::ErrorEvent e;
    e.source = helix::ErrorSource::IFS;
    e.severity = helix::ErrorSeverity::CRITICAL;
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

TEST_CASE_METHOD(LVGLUITestFixture, "AmsErrorBridge does nothing when current_error is null",
                 "[error-center][ams-bridge]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto backend = std::make_unique<ErrorReportingBackend>(4); // err_ defaults to nullopt
    ams.set_backend(std::move(backend));
    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::AmsErrorBridge bridge(presenter);
    bridge.start();
    ams.set_action(AmsAction::ERROR);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());
    ams.set_backend(nullptr);
}

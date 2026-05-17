// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_test_fixture.h"

#include "async_lifetime_guard.h"
#include "config.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "system_settings_manager.h"
#include "ui_modal.h"
#include "ui_test_utils.h"
#include "ui_update_queue.h"

HelixTestFixture::HelixTestFixture() {
    // Tests opt into strict L081 detection: any bg-thread tok.expired() check
    // while alive aborts the run instead of just warning. Production stays
    // at warn. See include/async_lifetime_guard.h.
    helix::internal::set_strict_bg_check(true);
    reset_all();
}

HelixTestFixture::~HelixTestFixture() {
    reset_all();
}

void HelixTestFixture::reset_all() {
    // LVGL + UpdateQueue must be up before we touch any subject-backed state.
    // lv_init_safe() is idempotent and also re-arms the UpdateQueue if a prior
    // fixture's destructor shut it down. Safe to call from non-LVGL tests.
    lv_init_safe();

    // Drain any callbacks queued by a prior test before we touch state they read.
    helix::ui::UpdateQueue::instance().drain();

    // SystemSettingsManager language back to "en" (matches config default).
    // init_subjects() is idempotent — first call creates the subjects, later
    // calls are no-ops. Required because set_language() writes to an LVGL subject.
    //
    // Force Config singleton creation — SystemSettingsManager::init_subjects() below
    // dereferences Config::get_instance() to read defaults.
    helix::Config::get_instance();
    helix::SystemSettingsManager::instance().init_subjects();
    helix::SystemSettingsManager::instance().set_language("en");

    // Delete any tracked modal widgets and clear the modal stack.
    ModalStack::instance().clear();

    // Destroy any lingering PrintStatusWidget::DetailedFormatter singleton.
    // PrintStatusWidget's ctor eagerly creates s_formatter_ (needed before XML
    // parse), and its dtor only decrements the refcount — by design, the
    // formatter survives widget destruction so helix-xml's global scope keeps
    // valid subject pointers in production. In tests that's a UAF trap:
    // subsequent tests calling PrinterStateTestAccess::reset(ps) deinit the
    // PrinterState subjects the formatter observes; lv_subject_deinit frees
    // the observer nodes, leaving the formatter's ObserverGuards with
    // dangling lv_observer_t* pointers. The next destructor that walks those
    // guards crashes on macOS (libc++/libmalloc is stricter than glibc).
    // Tearing the formatter down here — while the subjects it observes are
    // still alive — closes the window. No-op when no formatter exists, which
    // is the common case.
    helix::PrintStatusWidget::destroy_formatter_for_test();

    // NOTE: NavigationManager has no public reset API (clear_overlay_stack is
    // private; shutdown() is a one-way teardown for app exit). Add a reset
    // here if/when test flakiness from leftover panel/overlay state surfaces.
    //
    // NOTE: theme_manager has no "reset to default" entry point either. If
    // tests start mutating the active theme, add a reset alongside that work.
}

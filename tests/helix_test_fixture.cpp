// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_test_fixture.h"

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "async_lifetime_guard.h"
#include "config.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "system_settings_manager.h"
#include "test_helpers/print_control_buttons_test_access.h"

#include <cstdlib>

namespace {
// Force SDL's dummy audio driver for the WHOLE test binary, before any code can
// open a real device. SoundManager::create_backend() unconditionally constructs
// an SDLSoundBackend in HELIX_DISPLAY_SDL builds; on a developer box with a live
// PulseAudio/ALSA server, SDL_OpenAudioDevice() spins up a callback thread that
// can stall and wedge the long-running [slow] suite — the main thread then
// blocks on a futex at a non-deterministic point (looks like a random test
// hanging). CI runners have no audio device, so SDL_OpenAudioDevice() fast-fails
// and the singleton falls back; the dummy driver makes the suite behave that way
// everywhere instead of depending on the host's audio stack. This is a static
// initializer so it runs before main() — before the first SoundManager access.
// overwrite=0 lets a developer opt back into a real driver by exporting
// SDL_AUDIODRIVER themselves.
struct ForceDummyAudioDriver {
    ForceDummyAudioDriver() {
        ::setenv("SDL_AUDIODRIVER", "dummy", /*overwrite=*/0);
    }
};
const ForceDummyAudioDriver g_force_dummy_audio_driver;
} // namespace

HelixTestFixture::HelixTestFixture() {
    // Tests opt into strict L081 detection: any bg-thread tok.expired() check
    // while alive aborts the run instead of just warning. Production stays
    // at warn. See include/async_lifetime_guard.h.
    helix::internal::set_strict_bg_check(true);
    // Tests also opt into strict overlay-registration detection: any
    // push_overlay() on a widget that was never registered (so on_deactivate()
    // would never fire on dismiss) aborts instead of just warning. Production
    // stays at warn. See NavigationManager::set_overlay_registration_strict.
    NavigationManager::set_overlay_registration_strict(true);
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

    // Tear down the PrintControlButtons singleton for the same reason as the
    // formatter above. The controller persists across tests (it's a process
    // singleton) and observes the GLOBAL print_state_enum subject. A later test
    // calling PrinterStateTestAccess::reset(ps) — or process exit — deinits that
    // subject; lv_subject_deinit frees the observer node, leaving the
    // controller's ObserverGuard with a dangling lv_observer_t*. The next
    // destructor that walks it (the singleton's own static teardown at exit, or
    // the next fixture's reset()) calls lv_observer_remove() on freed memory →
    // SIGSEGV / heap corruption. This surfaced as nightly [slow] crashes:
    // a segfault in ~PrintControlButtons at process exit, plus mid-run
    // "malloc(): unaligned tcache chunk detected" aborts. Removing the observer
    // here — while print_state_enum is still alive — closes the window. No-op
    // when the controller was never initialized.
    helix::ui::PrintControlButtonsTestAccess::reset();

    // NOTE: NavigationManager has no public reset API (clear_overlay_stack is
    // private; shutdown() is a one-way teardown for app exit). Add a reset
    // here if/when test flakiness from leftover panel/overlay state surfaces.
    //
    // NOTE: theme_manager has no "reset to default" entry point either. If
    // tests start mutating the active theme, add a reset alongside that work.
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_idle_poweroff.cpp
 * @brief Tests for #1049 — two-stage idle on no-backlight devices + real power-off.
 *
 * Bug 1 (root cause): the Sleep>=Dim coupling in DisplaySettingsManager was only
 *        enforced when has_dimming_control() was true, so no-backlight devices
 *        (the reporter's Pi, Backlight-None) could persist Sleep < Dim and starve
 *        the screensaver stage. The coupling must also fire when a screensaver is
 *        enabled (the screensaver IS the dim-stage visual there). Verified via
 *        DisplaySettingsManager setters + init cross-validation, plus the awake
 *        path reaching the screensaver at the Dim timeout on a no-backlight device.
 *
 * Bug 2: When the screensaver is OFF and there is no hardware backlight blank,
 *        idle entry must perform a real, capability-gated display power-off
 *        (fbdev FBIOBLANK FB_BLANK_POWERDOWN) via the display backend, with the
 *        software overlay remaining the fallback when no power-off exists.
 *
 * In these tests no DisplayManager singleton is constructed, so
 * DisplaySettingsManager::has_dimming_control() returns false — i.e. the
 * no-backlight Pi case. screensaver_enabled() is driven via the screensaver_type
 * setting. The power-off seam is exercised through a fake DisplayBackend injected
 * via DisplayManager's test hooks.
 */

#include "backlight_backend.h"
#include "config.h"
#include "display_backend.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/display_manager_test_access.h"

#include "../../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

namespace {

// Minimal fake backend that records power_off()/power_on() and blank/unblank
// calls so the idle-entry path can be asserted without real hardware.
class FakePowerOffBackend : public DisplayBackend {
  public:
    explicit FakePowerOffBackend(bool supports_power_off,
                                 DisplayBackendType type = DisplayBackendType::FBDEV)
        : m_supports(supports_power_off), m_type(type) {}

    lv_display_t* create_display(int, int) override {
        return nullptr;
    }
    lv_indev_t* create_input_pointer() override {
        return nullptr;
    }
    DisplayBackendType type() const override {
        return m_type;
    }
    const char* name() const override {
        return "FakePowerOff";
    }
    bool is_available() const override {
        return true;
    }

    bool supports_power_off() const override {
        return m_supports;
    }
    bool power_off() override {
        ++power_off_calls;
        return m_supports;
    }
    bool power_on() override {
        ++power_on_calls;
        return m_supports;
    }

    bool blank_display() override {
        ++blank_calls;
        return true;
    }
    bool unblank_display() override {
        ++unblank_calls;
        return true;
    }

    int power_off_calls = 0;
    int power_on_calls = 0;
    int blank_calls = 0;
    int unblank_calls = 0;

  private:
    bool m_supports;
    DisplayBackendType m_type;
};

// Fake controllable backlight (e.g. the Snapmaker U1's Sysfs pwm-backlight):
// is_available() == true, set_brightness(0) cleanly turns the panel off.
class FakeBacklight : public BacklightBackend {
  public:
    explicit FakeBacklight(bool available) : m_available(available) {}

    bool set_brightness(int percent) override {
        last_brightness = percent;
        return m_available;
    }
    int get_brightness() const override {
        return m_available ? last_brightness : -1;
    }
    bool is_available() const override {
        return m_available;
    }
    const char* name() const override {
        return "FakeBacklight";
    }

    int last_brightness = 100;

  private:
    bool m_available;
};

} // namespace

// ============================================================================
// Bug 1 — Sleep>=Dim coupling must hold on no-backlight + screensaver devices
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "screensaver_enabled drives the Sleep>=Dim coupling on no-backlight devices",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();

    SECTION("screensaver ON: setting sleep < dim is clamped up to dim") {
        config->set<int>("/display/screensaver_type", 1); // Flying Toasters → enabled
        config->set<int>("/display/dim_sec", 600);
        config->set<int>("/display/sleep_sec", 600);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();

        // No DisplayManager → has_dimming_control() is false; the screensaver is
        // the only reason the coupling should fire.
        REQUIRE_FALSE(DisplaySettingsManager::instance().has_dimming_control());
        REQUIRE(DisplaySettingsManager::instance().screensaver_enabled());
        REQUIRE(DisplaySettingsManager::instance().should_couple_sleep_to_dim());

        // Reporter's exact mistake: sleep=60 < dim=600. Must be forced up to 600.
        DisplaySettingsManager::instance().set_display_sleep_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);
    }

    SECTION("screensaver OFF + no backlight: short sleep is NOT clamped") {
        config->set<int>("/display/screensaver_type", 0); // OFF
        config->set<int>("/display/dim_sec", 600);
        config->set<int>("/display/sleep_sec", 1200);
        DisplaySettingsManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().init_subjects();

        REQUIRE_FALSE(DisplaySettingsManager::instance().screensaver_enabled());
        REQUIRE_FALSE(DisplaySettingsManager::instance().should_couple_sleep_to_dim());

        // With neither dimming nor screensaver, a short sleep is legitimate
        // (e.g. AD5M/AD5X binary backlight) and must NOT be clamped.
        DisplaySettingsManager::instance().set_display_sleep_sec(60);
        REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 60);
    }

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "raising dim above sleep bumps sleep up when a screensaver is enabled",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1);
    config->set<int>("/display/dim_sec", 60);
    config->set<int>("/display/sleep_sec", 120);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    // Raising dim above the current sleep must bump sleep up to match.
    DisplaySettingsManager::instance().set_display_dim_sec(300);
    REQUIRE(DisplaySettingsManager::instance().get_display_dim_sec() == 300);
    REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 300);

    DisplaySettingsManager::instance().deinit_subjects();
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "init cross-validation self-corrects a persisted sleep<dim on no-backlight + screensaver",
    "[application][display][sleep][screensaver][1049]") {
    // Reproduces bundle 3VDWMLYQ exactly: no backlight, screensaver enabled,
    // persisted sleep=60 < dim=600. On the next launch (init_subjects) the
    // config must self-correct to sleep>=dim instead of starving the screensaver.
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1);
    config->set<int>("/display/dim_sec", 600);
    config->set<int>("/display/sleep_sec", 60); // inconsistent persisted value

    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    REQUIRE(DisplaySettingsManager::instance().get_display_sleep_sec() == 600);
    REQUIRE(config->get<int>("/display/sleep_sec", -1) == 600);

    DisplaySettingsManager::instance().deinit_subjects();
}

// ============================================================================
// Bug 1 — awake path reaches the screensaver at the Dim timeout (no backlight)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "awake path starts the screensaver at the Dim timeout on a no-backlight device",
                 "[application][display][sleep][screensaver][1049]") {
    helix::Config* config = helix::Config::get_instance();
    config->set<int>("/display/screensaver_type", 1); // enabled
    config->set<int>("/display/dim_sec", 600);
    config->set<int>("/display/sleep_sec", 1200);
    DisplaySettingsManager::instance().deinit_subjects();
    DisplaySettingsManager::instance().init_subjects();

    DisplayManager mgr; // no init() → m_backlight is null (no-backlight device)
    // Drive the dim stage deterministically with a 1s dim timeout; the sleep
    // timeout stays at 1200s so the sleep branch does not fire. With no backlight,
    // can_dim is false — the dim/screensaver branch is reached only because a
    // screensaver is enabled (the #1049 guard on display_manager.cpp:829-830).
    mgr.set_dim_timeout(1);

    // Advance LVGL's tick past the 1s dim timeout. In headless tests the tick is
    // manual (process_lvgl drives lv_tick_inc) — a wall-clock sleep would NOT
    // move lv_display_get_inactive_time(), so use process_lvgl, not delay().
    lv_display_trigger_activity(nullptr);
    process_lvgl(1300);

    REQUIRE_FALSE(mgr.is_display_sleeping());
    mgr.check_display_sleep();

    // The dim/screensaver branch must have been reached and the screensaver
    // started (display marked dimmed, not slept).
    REQUIRE(mgr.is_display_dimmed());
    REQUIRE_FALSE(mgr.is_display_sleeping());

    mgr.wake_display();
    DisplaySettingsManager::instance().deinit_subjects();
}

// ============================================================================
// Bug 2 — real display power-off seam (fbdev FB_BLANK_POWERDOWN)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep powers off the panel when backend supports it (screensaver OFF)",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Software-blank strategy (no hardware backlight blank), like the Pi/HDMI bundle.
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    // init() sets this from backend->supports_power_off(); mirror that here.
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 1);
    REQUIRE(raw->blank_calls == 0); // power-off is not the FB_BLANK_NORMAL hardware-blank path

    // Wake-side restore must power the panel back on (the full wake_display()
    // also calls lv_refr_now(), which can't run under the headless test display).
    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep falls back to software overlay when no power-off capability",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/false);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);
    // Backend reports no power-off capability → init() would leave this false.
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 0); // capability gate: not supported → not called

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 0); // nothing to power on — overlay was used
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "enter_sleep keeps hardware-blank path when backlight blanks (no power-off)",
                 "[application][display][sleep][poweroff][1049]") {
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Hardware-blank devices (AD5M/Allwinner) must keep using blank_display(),
    // not the new power_off() path.
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, true);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->blank_calls == 1);
    REQUIRE(raw->power_off_calls == 0);

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->unblank_calls == 1); // hardware-blank path unblanks, never power_on
    REQUIRE(raw->power_on_calls == 0);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "DRM backend power-off flows through the same generic seam (CB1/Pi HDMI)",
                 "[application][display][sleep][poweroff][drm][1049]") {
    // The reporter's Pi and the CB1 verification device render via DRM (DPMS is
    // the real power-off there). DisplayManager's power-off path is backend-type
    // agnostic — it calls m_backend->power_off()/power_on() regardless of type —
    // so a DRM-typed backend reporting supports_power_off() must drive exactly the
    // same path as the fbdev case. (The actual drmModeConnectorSetProperty DPMS
    // call lives in DisplayBackendDRM and can't run in CI without a DRM master.)
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false); // no backlight blank (CB1)
    DisplayManagerTestAccess::set_use_power_off(mgr, raw->supports_power_off());

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw->power_off_calls == 1); // DRM DPMS-off path, not the software overlay
    REQUIRE(raw->blank_calls == 0);

    DisplayManagerTestAccess::restore_display_output(mgr);
    REQUIRE(raw->power_on_calls == 1); // DPMS-on before lv_refr_now on wake (#303)
}

// ============================================================================
// Bug 2 — power-off gate: LAST RESORT only (Snapmaker U1 regression guard)
// ============================================================================

TEST_CASE("power-off gate: only with NO hardware blank AND NO usable backlight",
          "[application][display][sleep][poweroff][1049]") {
    // Pure decision matrix for should_use_power_off(use_hw_blank,
    // has_usable_backlight, backend_supports_power_off).

    // No hw blank, no backlight, backend can power off → power off (reporter HDMI / CB1).
    REQUIRE(DisplayManager::should_use_power_off(false, false, true));

    // Usable backlight present → NEVER power off, even with a DPMS-capable backend.
    // This is the Snapmaker U1 case: a working pwm backlight + a DPMS-capable DRM
    // connector. DRM DPMS-off there wedges the VOP2 CRTC permanently.
    REQUIRE_FALSE(DisplayManager::should_use_power_off(false, true, true));

    // Hardware blank present → never power off (AD5M/Allwinner) — unchanged.
    REQUIRE_FALSE(DisplayManager::should_use_power_off(true, false, true));

    // Backend can't power off → never (falls back to software overlay).
    REQUIRE_FALSE(DisplayManager::should_use_power_off(false, false, false));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "usable backlight suppresses power-off so enter_sleep dims instead (U1 guard)",
                 "[application][display][sleep][poweroff][1049]") {
    // U1: working pwm-backlight (is_available()==true) + a DPMS-capable DRM
    // backend that reports Hardware blank: false. Without the backlight clause in
    // the gate, m_use_power_off would become true and DRM DPMS-off would wedge the
    // VOP2 CRTC. With it, m_use_power_off MUST be false and enter_sleep MUST NOT
    // call power_off() — it turns the backlight off and uses the software overlay.
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw_backend = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    DisplayManagerTestAccess::set_backlight(mgr,
                                            std::make_unique<FakeBacklight>(/*available=*/true));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false); // U1 reports no hw blank

    // Run the real init() gate against the injected state.
    bool use_power_off = DisplayManagerTestAccess::compute_use_power_off(mgr);
    REQUIRE_FALSE(use_power_off); // the regression guard

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw_backend->power_off_calls == 0); // never DPMS-off a backlight device
    REQUIRE(raw_backend->blank_calls == 0);     // not a hardware-blank device either
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "no usable backlight keeps power-off enabled (reporter HDMI / CB1)",
                 "[application][display][sleep][poweroff][1049]") {
    // Backlight-None device with a DPMS-capable backend: power-off is the only way
    // to cut the panel, so the gate must KEEP it enabled.
    DisplayManager mgr;
    auto backend = std::make_unique<FakePowerOffBackend>(/*supports_power_off=*/true,
                                                         DisplayBackendType::DRM);
    FakePowerOffBackend* raw_backend = backend.get();
    DisplayManagerTestAccess::set_backend(mgr, std::move(backend));
    // Backlight present but NOT available (Backlight-None in production).
    DisplayManagerTestAccess::set_backlight(mgr,
                                            std::make_unique<FakeBacklight>(/*available=*/false));
    DisplayManagerTestAccess::set_use_hardware_blank(mgr, false);

    bool use_power_off = DisplayManagerTestAccess::compute_use_power_off(mgr);
    REQUIRE(use_power_off);

    DisplayManagerTestAccess::enter_sleep(mgr, 60);

    REQUIRE(mgr.is_display_sleeping());
    REQUIRE(raw_backend->power_off_calls == 1); // power-off still used on no-backlight devices
}

// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "display_settings_manager.h"
#include "screensaver.h"
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

ScreensaverManager& ScreensaverManager::instance() {
    static ScreensaverManager mgr;
    return mgr;
}

ScreensaverManager::ScreensaverManager() {
    // Register all screensaver implementations
    screensavers_.push_back(std::make_unique<FlyingToasterScreensaver>());
    screensavers_.push_back(std::make_unique<StarfieldScreensaver>());
    screensavers_.push_back(std::make_unique<PipesScreensaver>());
}

void ScreensaverManager::start(ScreensaverType type) {
    if (type == ScreensaverType::OFF) {
        stop();
        return;
    }

    // Stop current screensaver if different type is requested
    if (active_ && active_->type() != type) {
        active_->stop();
        active_ = nullptr;
    }

    // Already running the requested type
    if (active_ && active_->is_active()) {
        return;
    }

    Screensaver* ss = find(type);
    if (!ss) {
        spdlog::warn("[ScreensaverManager] No screensaver registered for type {}",
                     static_cast<int>(type));
        return;
    }

    ss->start();
    active_ = ss;
    spdlog::info("[ScreensaverManager] Started screensaver type {}", static_cast<int>(type));
}

void ScreensaverManager::stop() {
    if (active_) {
        active_->stop();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {}",
                     static_cast<int>(active_->type()));
        active_ = nullptr;
    }
}

bool ScreensaverManager::is_active() const {
    return active_ && active_->is_active();
}

ScreensaverType ScreensaverManager::configured_type() {
    int type_int = helix::DisplaySettingsManager::instance().get_screensaver_type();
    if (type_int < 0 || type_int > 3) {
        return ScreensaverType::OFF;
    }
    return static_cast<ScreensaverType>(type_int);
}

Screensaver* ScreensaverManager::find(ScreensaverType type) const {
    for (auto& ss : screensavers_) {
        if (ss->type() == type) {
            return ss.get();
        }
    }
    return nullptr;
}

#endif // HELIX_ENABLE_SCREENSAVER

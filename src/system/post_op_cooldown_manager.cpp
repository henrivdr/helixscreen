// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "post_op_cooldown_manager.h"

#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "temperature_controller.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

PostOpCooldownManager& PostOpCooldownManager::instance() {
    static PostOpCooldownManager inst;
    return inst;
}

void PostOpCooldownManager::init(MoonrakerAPI* api) {
    api_ = api;
    initialized_ = true;
    spdlog::info("[PostOpCooldown] Initialized");
}

void PostOpCooldownManager::schedule() {
    if (!initialized_) {
        spdlog::warn("[PostOpCooldown] schedule() called before init");
        return;
    }

    auto* cfg = helix::Config::get_instance();
    int delay_seconds =
        cfg ? cfg->get<int>(cfg->df() + "filament/cooldown_delay_seconds", 120) : 120;

    spdlog::info("[PostOpCooldown] Scheduling cooldown in {}s", delay_seconds);

    helix::ui::queue_update([this, delay_seconds]() {
        // Delete existing timer if any
        if (timer_) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }

        timer_ = lv_timer_create(
            [](lv_timer_t* /*t*/) {
                auto& self = PostOpCooldownManager::instance();
                self.timer_ = nullptr;

                auto& state = get_printer_state();

                // Skip if printing or paused
                auto job_state = state.get_print_job_state();
                if (job_state == helix::PrintJobState::PRINTING ||
                    job_state == helix::PrintJobState::PAUSED) {
                    spdlog::info("[PostOpCooldown] Skipping cooldown — print active");
                    return;
                }

                // Check extruder target (centidegrees, > 0 means heater is on)
                auto* target_subj = state.get_active_extruder_target_subject();
                if (!target_subj || lv_subject_get_int(target_subj) == 0) {
                    spdlog::debug("[PostOpCooldown] Skipping cooldown — extruder already off");
                    return;
                }

                spdlog::info("[PostOpCooldown] Turning off extruder heater ({})",
                             state.active_extruder_name());
                if (auto* c = get_temperature_controller()) {
                    c->set_target(helix::HeaterType::Nozzle, 0, {.toast = false});
                }
            },
            static_cast<uint32_t>(delay_seconds) * 1000, nullptr);
        lv_timer_set_repeat_count(timer_, 1);
    });
}

void PostOpCooldownManager::cancel() {
    if (!initialized_) return;

    spdlog::debug("[PostOpCooldown] Cancelling pending cooldown");

    helix::ui::queue_update([this]() {
        if (timer_) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
    });
}

void PostOpCooldownManager::shutdown() {
    if (!initialized_) return;

    spdlog::info("[PostOpCooldown] Shutting down");

    // Shutdown runs on main thread, so we can delete directly
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    api_ = nullptr;
    initialized_ = false;
}

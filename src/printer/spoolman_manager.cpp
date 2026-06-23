// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file spoolman_manager.cpp
 * @brief Centralized Spoolman weight polling and circuit breaker management
 *
 * @pattern Singleton with static s_shutdown_flag atomic for callback safety
 * @threading Weight refresh callbacks arrive from HTTP thread; circuit breaker
 *            state is updated on UI thread via queue_update
 *
 * @see ams_state.cpp (slot data remains in AmsState)
 */

#include "spoolman_manager.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

using namespace helix;

// Shutdown flag to prevent async callbacks from accessing destroyed singleton
std::atomic<bool> SpoolmanManager::s_shutdown_flag{false};

SpoolmanManager& SpoolmanManager::instance() {
    static SpoolmanManager inst;
    return inst;
}

SpoolmanManager::~SpoolmanManager() {
    // Signal shutdown to prevent async callbacks from accessing this instance
    s_shutdown_flag.store(true, std::memory_order_release);

    // Clean up poll timer if still active (check LVGL is initialized
    // to avoid crash during static destruction order issues)
    if (poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
}

void SpoolmanManager::init_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (initialized_) {
        return;
    }

    // Clear shutdown flag — supports soft restart (printer switching)
    s_shutdown_flag.store(false, std::memory_order_release);

    spdlog::trace("[SpoolmanManager] Initializing subjects");

    // Observe print state changes to auto-refresh Spoolman weights.
    // Refreshes when print starts, ends, or pauses to keep weight data current.
    using helix::ui::observe_int_sync;
    print_state_observer_ = observe_int_sync<SpoolmanManager>(
        get_printer_state().get_print_state_enum_subject(), this,
        [](SpoolmanManager* self, int state) {
            auto print_state = static_cast<PrintJobState>(state);
            if (print_state == PrintJobState::PRINTING || print_state == PrintJobState::COMPLETE ||
                print_state == PrintJobState::PAUSED) {
                spdlog::debug(
                    "[SpoolmanManager] Print state changed to {}, refreshing Spoolman weights",
                    static_cast<int>(print_state));
                self->refresh_spoolman_weights();
            }
        });

    // Observe Spoolman availability — force-stop polling when Spoolman disappears
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (spoolman_subj) {
        spoolman_availability_observer_ = observe_int_sync<SpoolmanManager>(
            spoolman_subj, this, [](SpoolmanManager* self, int value) {
                if (value == 0) {
                    std::lock_guard<std::recursive_mutex> lock(self->mutex_);
                    spdlog::info("[SpoolmanManager] Spoolman became unavailable, stopping polling");
                    self->poll_refcount_ = 0;
                    if (self->poll_timer_ && lv_is_initialized()) {
                        lv_timer_delete(self->poll_timer_);
                        self->poll_timer_ = nullptr;
                    }
                    self->reset_circuit_breaker();
                }
            });
    }

    initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "SpoolmanManager", []() { SpoolmanManager::instance().deinit_subjects(); });
}

void SpoolmanManager::deinit_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    spdlog::trace("[SpoolmanManager] Deinitializing subjects");

    s_shutdown_flag.store(true, std::memory_order_release);

    // Release cross-singleton observers — they observe subjects from PrinterState
    // which may already be destroyed during StaticSubjectRegistry::deinit_all()
    // reverse-order teardown. Using release() (not reset()) avoids dereferencing
    // a dangling subject pointer in lv_observer_remove().
    print_state_observer_.release();
    spoolman_availability_observer_.release();

    // Clear dangling API pointer — MoonrakerAPI is destroyed during teardown
    api_ = nullptr;

    if (poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }

    initialized_ = false;
}

void SpoolmanManager::set_api(MoonrakerAPI* api) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    api_ = api;
    reset_circuit_breaker();
}

void SpoolmanManager::refresh_spoolman_weights() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Mock backends use fake spoolman IDs that don't exist in real Spoolman
    if (get_runtime_config()->should_mock_ams()) {
        return;
    }

    if (!api_) {
        return;
    }

    // Skip if Spoolman is not configured/connected in Moonraker
    if (!get_printer_state().is_spoolman_available()) {
        spdlog::trace("[SpoolmanManager] Spoolman not available, skipping weight refresh");
        return;
    }

    uint32_t now = lv_tick_get();

    // Circuit breaker: if open, check if backoff period has elapsed
    if (cb_open_) {
        uint32_t elapsed = now - cb_tripped_at_ms_;
        if (elapsed < CB_BACKOFF_MS) {
            spdlog::trace("[SpoolmanManager] Spoolman circuit breaker open, {}ms remaining",
                          CB_BACKOFF_MS - elapsed);
            return;
        }
        // Backoff elapsed — half-open: allow one probe request through
        spdlog::info("[SpoolmanManager] Spoolman circuit breaker half-open, probing...");
        cb_open_ = false;
    }

    // Debounce: skip if called too recently
    if (last_refresh_ms_ > 0) {
        uint32_t since_last = now - last_refresh_ms_;
        if (since_last < DEBOUNCE_MS) {
            spdlog::trace("[SpoolmanManager] Spoolman refresh debounced ({}ms since last)",
                          since_last);
            return;
        }
    }
    last_refresh_ms_ = now;

    int linked_count = 0;

    // Refresh AMS backend slots (if a backend is active)
    auto* backend = AmsState::instance().get_backend(0);
    if (backend) {
        // When the backend tracks weight locally (e.g., AFC decrements weight
        // via extruder position), we still need total_weight_g (initial weight)
        // from Spoolman — the backend only provides remaining weight.
        bool local_weight = backend->tracks_weight_locally();
        int slot_count = backend->get_system_info().total_slots;

        for (int i = 0; i < slot_count; ++i) {
            SlotInfo slot = backend->get_slot_info(i);
            if (slot.spoolman_id > 0) {
                ++linked_count;
                int slot_index = i;
                int spoolman_id = slot.spoolman_id;

                api_->spoolman().get_spoolman_spool(
                    spoolman_id,
                    [slot_index, spoolman_id,
                     local_weight](const std::optional<SpoolInfo>& spool_opt) {
                        if (!spool_opt.has_value()) {
                            spdlog::warn("[SpoolmanManager] Spoolman spool {} not found",
                                         spoolman_id);
                            return;
                        }

                        const SpoolInfo& spool = spool_opt.value();

                        // Data to pass to UI thread
                        struct WeightUpdate {
                            int slot_index;
                            int expected_spoolman_id; // To verify slot wasn't reassigned
                            float remaining_weight_g;
                            float total_weight_g;
                            bool local_weight; // Backend tracks remaining weight locally
                        };

                        auto update_data = std::make_unique<WeightUpdate>(WeightUpdate{
                            slot_index, spoolman_id, static_cast<float>(spool.remaining_weight_g),
                            static_cast<float>(spool.initial_weight_g), local_weight});

                        helix::ui::queue_update<
                            WeightUpdate>(std::move(update_data), [](WeightUpdate* d) {
                            // Skip if shutdown is in progress
                            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                                return;
                            }

                            SpoolmanManager& mgr = SpoolmanManager::instance();
                            std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                            // Success response — reset circuit breaker (on UI thread)
                            if (mgr.consecutive_failures_ > 0) {
                                spdlog::info(
                                    "[SpoolmanManager] Spoolman recovered after {} failures",
                                    mgr.consecutive_failures_);
                            }
                            mgr.consecutive_failures_ = 0;
                            mgr.unavailable_notified_ = false;

                            AmsState& ams = AmsState::instance();
                            auto* primary = ams.get_backend(0);
                            if (!primary) {
                                return;
                            }

                            // Get current slot info and verify it wasn't reassigned
                            SlotInfo slot = primary->get_slot_info(d->slot_index);
                            if (slot.spoolman_id != d->expected_spoolman_id) {
                                spdlog::debug(
                                    "[SpoolmanManager] Slot {} spoolman_id changed ({} -> {}), "
                                    "skipping stale weight update",
                                    d->slot_index, d->expected_spoolman_id, slot.spoolman_id);
                                return;
                            }

                            // When backend tracks weight locally, only update total_weight
                            // (initial weight from Spoolman). Preserve the backend's
                            // remaining_weight which is more accurate than Spoolman's.
                            float new_remaining =
                                d->local_weight ? slot.remaining_weight_g : d->remaining_weight_g;

                            // Skip update if weights haven't changed (avoids UI refresh cascade)
                            if (slot.remaining_weight_g == new_remaining &&
                                slot.total_weight_g == d->total_weight_g) {
                                spdlog::trace("[SpoolmanManager] Slot {} weights unchanged "
                                              "({:.0f}g / {:.0f}g)",
                                              d->slot_index, new_remaining, d->total_weight_g);
                                return;
                            }

                            // Update weights and set back.
                            // CRITICAL: persist=false prevents an infinite feedback loop.
                            // With persist=true, set_slot_info sends G-code to firmware
                            // (e.g., SET_WEIGHT for AFC, MMU_GATE_MAP for Happy Hare).
                            // Firmware then emits a status_update WebSocket event, which
                            // triggers sync_from_backend -> refresh_spoolman_weights ->
                            // set_slot_info again, ad infinitum. With 4 AFC lanes this
                            // fires 16+ G-code commands per cycle and saturates the CPU.
                            // Since these weights come FROM Spoolman (an external source),
                            // there's no need to write them back to firmware.
                            slot.remaining_weight_g = new_remaining;
                            slot.total_weight_g = d->total_weight_g;
                            primary->set_slot_info(d->slot_index, slot, /*persist=*/false);
                            ams.bump_slots_version();

                            spdlog::debug(
                                "[SpoolmanManager] Updated slot {} weights: {:.0f}g / {:.0f}g{}",
                                d->slot_index, new_remaining, d->total_weight_g,
                                d->local_weight ? " (local remaining)" : "");
                        });
                    },
                    [spoolman_id](const MoonrakerError& err) {
                        spdlog::warn("[SpoolmanManager] Failed to fetch Spoolman spool {}: {}",
                                     spoolman_id, err.message);

                        // Track failure for circuit breaker (post to UI thread for
                        // thread-safe access to SpoolmanManager and ToastManager)
                        helix::ui::queue_update([]() {
                            if (s_shutdown_flag.load(std::memory_order_acquire)) {
                                return;
                            }

                            SpoolmanManager& mgr = SpoolmanManager::instance();
                            std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);

                            mgr.consecutive_failures_++;

                            if (mgr.consecutive_failures_ >= CB_FAILURE_THRESHOLD) {
                                mgr.cb_open_ = true;
                                mgr.cb_tripped_at_ms_ = lv_tick_get();
                                spdlog::warn(
                                    "[SpoolmanManager] Spoolman circuit breaker OPEN after {} "
                                    "failures, backing off {}s",
                                    mgr.consecutive_failures_, CB_BACKOFF_MS / 1000);

                                // Notify user once per outage — only if Spoolman is
                                // actually configured (avoid confusing toast on printers
                                // that never set up Spoolman)
                                if (!mgr.unavailable_notified_) {
                                    mgr.unavailable_notified_ = true;
                                    auto* subj =
                                        lv_xml_get_subject(nullptr, "printer_has_spoolman");
                                    if (subj && lv_subject_get_int(subj) == 1) {
                                        // i18n: Spoolman is a product name, do not translate
                                        ToastManager::instance().show(
                                            ToastSeverity::WARNING,
                                            lv_tr("Spoolman unavailable — filament weights "
                                                  "may be stale"),
                                            6000);
                                    }
                                }
                            }
                        });
                    },
                    /*silent=*/true);
            }
        }
    } // if (backend)

    // Also refresh external spool if it has a Spoolman link
    auto ext_spool = AmsState::instance().get_external_spool_info();
    if (ext_spool.has_value() && ext_spool->spoolman_id > 0) {
        ++linked_count;
        int ext_spoolman_id = ext_spool->spoolman_id;

        api_->spoolman().get_spoolman_spool(
            ext_spoolman_id,
            [ext_spoolman_id](const std::optional<SpoolInfo>& spool_opt) {
                if (!spool_opt.has_value()) {
                    spdlog::warn("[SpoolmanManager] External spool Spoolman #{} not found",
                                 ext_spoolman_id);
                    return;
                }

                const SpoolInfo& spool = spool_opt.value();
                float new_remaining = static_cast<float>(spool.remaining_weight_g);
                float new_total = static_cast<float>(spool.initial_weight_g);

                helix::ui::queue_update([ext_spoolman_id, new_remaining, new_total]() {
                    if (s_shutdown_flag.load(std::memory_order_acquire)) {
                        return;
                    }

                    AmsState& state = AmsState::instance();
                    auto ext = state.get_external_spool_info();
                    if (!ext.has_value() || ext->spoolman_id != ext_spoolman_id) {
                        spdlog::debug(
                            "[SpoolmanManager] External spool changed, skipping stale update");
                        return;
                    }

                    // Skip if weights unchanged
                    if (ext->remaining_weight_g == new_remaining &&
                        ext->total_weight_g == new_total) {
                        return;
                    }

                    ext->remaining_weight_g = new_remaining;
                    ext->total_weight_g = new_total;
                    state.set_external_spool_info(*ext);

                    spdlog::debug(
                        "[SpoolmanManager] Updated external spool weights: {:.0f}g / {:.0f}g",
                        new_remaining, new_total);
                });
            },
            [ext_spoolman_id](const MoonrakerError& err) {
                spdlog::warn("[SpoolmanManager] Failed to fetch external spool Spoolman #{}: {}",
                             ext_spoolman_id, err.message);
            },
            /*silent=*/true);
    }

    if (linked_count > 0) {
        spdlog::trace("[SpoolmanManager] Refreshing Spoolman weights for {} linked slots",
                      linked_count);
    }
}

void SpoolmanManager::reset_circuit_breaker() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    last_refresh_ms_ = 0;
    consecutive_failures_ = 0;
    cb_tripped_at_ms_ = 0;
    cb_open_ = false;
    unavailable_notified_ = false;
}

void SpoolmanManager::start_spoolman_polling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!get_printer_state().is_spoolman_available()) {
        spdlog::trace("[SpoolmanManager] Spoolman not available, skipping poll start");
        return;
    }

    ++poll_refcount_;
    spdlog::debug("[SpoolmanManager] Starting Spoolman polling (refcount: {})", poll_refcount_);

    // Only create timer on first reference
    if (poll_refcount_ == 1 && !poll_timer_) {
        poll_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* self = static_cast<SpoolmanManager*>(lv_timer_get_user_data(timer));
                self->refresh_spoolman_weights();
            },
            POLL_INTERVAL_MS, this);

        // Also do an immediate refresh
        refresh_spoolman_weights();
    }
}

void SpoolmanManager::stop_spoolman_polling() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (poll_refcount_ > 0) {
        --poll_refcount_;
    }

    spdlog::debug("[SpoolmanManager] Stopping Spoolman polling (refcount: {})", poll_refcount_);

    // Only delete timer when refcount reaches zero
    // Guard against LVGL already being deinitialized during shutdown
    if (poll_refcount_ == 0 && poll_timer_ && lv_is_initialized()) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "active_print_media_manager.h"

#include "ui_filename_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "gcode_parser.h"
#include "json_utils.h"
#include "memory_monitor.h"
#include "observer_factory.h"
#include "thumbnail_cache.h"
#include "thumbnail_processor.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>

using helix::gcode::get_display_filename;
using helix::gcode::resolve_gcode_filename;

namespace helix {

// Singleton storage
static std::unique_ptr<ActivePrintMediaManager> g_instance;

void init_active_print_media_manager() {
    if (g_instance) {
        spdlog::warn("[ActivePrintMediaManager] Already initialized");
        return;
    }
    g_instance = std::make_unique<ActivePrintMediaManager>(::get_printer_state());
    spdlog::debug("[ActivePrintMediaManager] Initialized");
}

void deinit_active_print_media_manager() {
    g_instance.reset();
    spdlog::debug("[ActivePrintMediaManager] Deinitialized");
}

ActivePrintMediaManager& get_active_print_media_manager() {
    if (!g_instance) {
        throw std::runtime_error("ActivePrintMediaManager not initialized");
    }
    return *g_instance;
}

ActivePrintMediaManager::ActivePrintMediaManager(PrinterState& printer_state)
    : printer_state_(printer_state) {
    // Observe print_filename_ subject to react to filename changes.
    // Use observe_string_immediate so process_filename runs SYNCHRONOUSLY
    // when the subject changes. This is critical: process_filename clears
    // the stale print_thumbnail_path_ from the previous print BEFORE any
    // deferred observers fire (e.g., print_start_navigation's push_overlay
    // → on_activate, which reads print_thumbnail_path_ to populate the UI).
    // Without this, the race window allows stale thumbnails to be cached
    // and displayed for the wrong print.
    // Safety: process_filename only clears subjects, queues updates, and
    // starts async operations — no observer lifecycle changes or widget
    // destruction, so immediate dispatch is safe.
    print_filename_observer_ = helix::ui::observe_string_immediate<ActivePrintMediaManager>(
        printer_state_.get_print_filename_subject(), this,
        [](ActivePrintMediaManager* self, const char* filename) {
            self->process_filename(filename);
        });

    spdlog::debug("[ActivePrintMediaManager] Observer attached to print_filename subject");
}

ActivePrintMediaManager::~ActivePrintMediaManager() {
    // ObserverGuard handles cleanup automatically
    // NOTE: No logging here - spdlog may be destroyed before this singleton
    cancel_thumbnail_retry();
    unregister_moonraker_listeners();
}

void ActivePrintMediaManager::set_api(MoonrakerAPI* api) {
    if (api_ != api) {
        unregister_moonraker_listeners();
    }
    api_ = api;
    spdlog::debug("[ActivePrintMediaManager] API set: {}", api ? "valid" : "nullptr");
    if (api_) {
        register_moonraker_listeners();
    }
}

void ActivePrintMediaManager::set_thumbnail_source(const std::string& original_filename) {
    thumbnail_source_filename_ = original_filename;
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source set to: {}",
                  original_filename.empty() ? "(cleared)" : original_filename);

    // If we have a current print filename, re-process it with the new source
    const char* current = lv_subject_get_string(printer_state_.get_print_filename_subject());
    if (current && current[0] != '\0' && !original_filename.empty()) {
        spdlog::info("[ActivePrintMediaManager] Re-processing with source override: {}",
                     original_filename);
        process_filename(current);
    }
}

void ActivePrintMediaManager::clear_thumbnail_source() {
    thumbnail_source_filename_.clear();
    last_effective_filename_.clear();
    last_loaded_thumbnail_filename_.clear();
    cancel_thumbnail_retry();
    spdlog::debug("[ActivePrintMediaManager] Thumbnail source cleared");
}

void ActivePrintMediaManager::set_thumbnail_path(const std::string& path) {
    // Set the thumbnail path directly (bypasses Moonraker API lookup)
    printer_state_.set_print_thumbnail_path(path);
    // A pre-extracted thumbnail (USB / embedded G-code) counts as loaded for
    // retry purposes; an empty path clears that state.
    thumbnail_loaded_ = !path.empty();
    spdlog::debug("[ActivePrintMediaManager] Thumbnail path set directly: {}", path);
}

void ActivePrintMediaManager::process_filename(const char* raw_filename) {
    // Empty filename means print ended or idle - DON'T clear immediately
    // The thumbnail/metadata should persist so the user can see what was printing
    // (especially after cancel→firmware_restart where Klipper reports empty filename)
    // Clearing will happen naturally when a NEW print starts with a different filename
    if (!raw_filename || raw_filename[0] == '\0') {
        if (!last_was_empty_) {
            spdlog::debug("[ActivePrintMediaManager] Filename empty - preserving current display");
            last_was_empty_ = true;
        }
        return;
    }
    last_was_empty_ = false;
    helix::MemoryMonitor::log_now("active_media_process_filename", spdlog::level::debug);

    std::string filename = raw_filename;

    // Auto-resolve temp file patterns to original filename if no override is set
    std::string resolved = resolve_gcode_filename(filename);
    if (resolved != filename && thumbnail_source_filename_.empty()) {
        spdlog::debug("[ActivePrintMediaManager] Auto-resolved temp filename: {} -> {}", filename,
                      resolved);
        thumbnail_source_filename_ = resolved;
    }

    // Compute effective filename (respects thumbnail_source override)
    std::string effective_filename =
        thumbnail_source_filename_.empty() ? filename : thumbnail_source_filename_;

    // Skip if effective filename hasn't changed (makes processing idempotent)
    if (effective_filename == last_effective_filename_) {
        return;
    }
    last_effective_filename_ = effective_filename;

    // Update display filename subject
    std::string display_name = get_display_filename(effective_filename);
    spdlog::debug("[ActivePrintMediaManager] Display filename: {}", display_name);

    // Thread-safe update to display filename subject (RAII via unique_ptr)
    // Capture printer_state_ reference to avoid using global in tests
    PrinterState* state = &printer_state_;
    helix::ui::queue_update<std::string>(
        std::make_unique<std::string>(display_name),
        [state](std::string* name) { state->set_print_display_filename(*name); });

    // Load thumbnail if filename changed
    if (!effective_filename.empty() && effective_filename != last_loaded_thumbnail_filename_) {
        // Clear stale thumbnail path from the previous print so load_thumbnail_for_file()
        // doesn't short-circuit with the old thumbnail. This fixes the bug where starting
        // a new print via Mainsail would show the previous print's thumbnail.
        // Only clear if we previously loaded a thumbnail for a different file — if
        // last_loaded_thumbnail_filename_ is empty, this is the first print and any
        // existing thumbnail was intentionally pre-set (e.g., USB/PrintStartController).
        if (!last_loaded_thumbnail_filename_.empty()) {
            printer_state_.set_print_thumbnail_path("");
        }
        // New file: drop any pending retry for the previous file and reset
        // the per-filename retry budget.
        cancel_thumbnail_retry();
        thumbnail_retry_count_ = 0;
        thumbnail_loaded_ = false;
        load_thumbnail_for_file(effective_filename);
        last_loaded_thumbnail_filename_ = effective_filename;
    }
}

void ActivePrintMediaManager::load_thumbnail_for_file(const std::string& filename) {
    // Check if thumbnail is already set (e.g., PrintStartController set it from USB).
    // We still need metadata for layer_count and estimated_time, so don't early-return.
    const char* current_thumb =
        lv_subject_get_string(printer_state_.get_print_thumbnail_path_subject());
    bool skip_thumbnail = (current_thumb && current_thumb[0] != '\0');
    if (skip_thumbnail) {
        spdlog::debug(
            "[ActivePrintMediaManager] Thumbnail already set ({}), will fetch metadata only",
            current_thumb);
        // A pre-set path (USB / PrintStartController) counts as loaded — the
        // notification re-triggers only care about a MISSING thumbnail.
        thumbnail_loaded_ = true;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[ActivePrintMediaManager] No API available - skipping thumbnail load");
        return;
    }

    // Increment generation to invalidate any in-flight async operations
    // (only after early-return checks to avoid incrementing when no async op starts).
    // Relaxed: the value is only compared for equality across loads; the actual
    // happens-before ordering of the apply is provided by the UpdateQueue.
    uint32_t current_gen = thumbnail_load_generation_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Resolve to original filename if this is a modified temp file
    // (Moonraker only has metadata for original files, not modified copies)
    std::string metadata_filename = resolve_gcode_filename(filename);

    spdlog::debug("[ActivePrintMediaManager] Loading metadata for: {}", metadata_filename);

    // Get file metadata for layer count, estimated time, and optionally thumbnail.
    //
    // THREADING: get_file_metadata invokes its success callback on the Moonraker
    // WebSocket background thread. The ONLY work allowed on that thread is
    // this-free local parsing — everything that touches `this`, `printer_state_`,
    // `api_`, the generation counter, or any subject is marshalled to the main
    // thread via tok.defer(). The metadata struct is copied by value into the
    // deferred body so the bg-thread reference can't dangle.
    api_->files().get_file_metadata(
        metadata_filename,
        [this, tok = lifetime_.token(), current_gen, skip_thumbnail, filename,
         metadata_filename](const FileMetadata& metadata) {
            // bg thread: copy the metadata, then marshal ALL member access to main.
            tok.defer("ActivePrintMediaManager::on_metadata", [this, current_gen, skip_thumbnail,
                                                               filename, metadata_filename,
                                                               metadata]() {
                // main thread, `this` valid + lifetime-checked.
                // Drop stale callbacks superseded by a newer load.
                if (current_gen != thumbnail_load_generation_.load(std::memory_order_relaxed)) {
                    spdlog::trace("[ActivePrintMediaManager] Stale metadata callback (gen {} != "
                                  "{}), ignoring",
                                  current_gen,
                                  thumbnail_load_generation_.load(std::memory_order_relaxed));
                    return;
                }

                // Set total layer count from metadata
                if (metadata.layer_count > 0) {
                    printer_state_.set_print_layer_total(static_cast<int>(metadata.layer_count));
                    spdlog::debug("[ActivePrintMediaManager] Set total layers from metadata: {}",
                                  metadata.layer_count);
                } else if (lv_subject_get_int(printer_state_.get_print_layer_total_subject()) > 0) {
                    // Retry pass: an earlier attempt already filled the layer
                    // total (gcode header scan) — don't re-download the header.
                    spdlog::debug("[ActivePrintMediaManager] Layer total already set, "
                                  "skipping gcode header re-scan");
                } else {
                    // Moonraker didn't provide layer count — scan gcode header directly.
                    // Download the first 16KB and parse slicer comments for layer info.
                    // Started on the main thread; its bg callback follows the same pattern.
                    spdlog::info("[ActivePrintMediaManager] No layer count in metadata, "
                                 "scanning gcode header");
                    bool need_est_time = (metadata.estimated_time <= 0);
                    api_->transfers().download_file_partial(
                        "gcodes", metadata_filename, 16 * 1024,
                        [this, tok = lifetime_.token(), current_gen,
                         need_est_time](const std::string& content) {
                            // bg thread (HttpExecutor::slow worker): parse locally only.
                            auto header =
                                helix::gcode::extract_header_metadata_from_content(content);
                            // main thread: re-check generation + apply.
                            tok.defer("ActivePrintMediaManager::on_gcode_header",
                                      [this, current_gen, need_est_time, header]() {
                                          if (current_gen != thumbnail_load_generation_.load(
                                                                 std::memory_order_relaxed)) {
                                              return;
                                          }
                                          if (header.layer_count > 0) {
                                              printer_state_.set_print_layer_total(
                                                  static_cast<int>(header.layer_count));
                                              spdlog::info("[ActivePrintMediaManager] Set total "
                                                           "layers from gcode header: {}",
                                                           header.layer_count);
                                          }
                                          if (need_est_time && header.estimated_time_seconds > 0) {
                                              printer_state_.set_estimated_print_time(
                                                  static_cast<int>(header.estimated_time_seconds));
                                              spdlog::info(
                                                  "[ActivePrintMediaManager] Set estimated "
                                                  "time from gcode header: {}s",
                                                  static_cast<int>(header.estimated_time_seconds));
                                          }
                                      });
                        },
                        [](const MoonrakerError& err) {
                            spdlog::debug("[ActivePrintMediaManager] Gcode header fetch failed: {}",
                                          err.message);
                        });
                }

                // Store slicer's estimated print time for remaining time fallback
                if (metadata.estimated_time > 0) {
                    printer_state_.set_estimated_print_time(
                        static_cast<int>(metadata.estimated_time));
                    spdlog::debug(
                        "[ActivePrintMediaManager] Set estimated print time from metadata: {}s",
                        metadata.estimated_time);
                }

                // Skip thumbnail fetch if one is already set
                if (skip_thumbnail) {
                    spdlog::debug(
                        "[ActivePrintMediaManager] Skipping thumbnail fetch (already set)");
                    return;
                }

                // Get the largest thumbnail available
                std::string thumbnail_rel_path = metadata.get_largest_thumbnail();
                if (thumbnail_rel_path.empty()) {
                    // Metadata record exists but has no thumbnails. Briefly this
                    // can mean Moonraker is still mid-scan, but the common cause
                    // is a file sliced WITHOUT thumbnails — a permanent
                    // condition. Retry only kMaxEmptyThumbnailRetries times and
                    // warn once; filelist_changed/klippy_ready re-triggers cover
                    // the late-scan case beyond that.
                    spdlog::log(thumbnail_retry_count_ == 0 ? spdlog::level::warn
                                                            : spdlog::level::debug,
                                "[ActivePrintMediaManager] No thumbnail in metadata for '{}' "
                                "(attempt {}/{}) - file may lack thumbnails or scan is "
                                "incomplete",
                                metadata_filename, thumbnail_retry_count_ + 1,
                                kMaxEmptyThumbnailRetries + 1);
                    schedule_thumbnail_retry(filename, kMaxEmptyThumbnailRetries);
                    return;
                }

                spdlog::debug("[ActivePrintMediaManager] Found thumbnail: {}", thumbnail_rel_path);

                // Use detail-sized thumbnails (200-400px) — works for both card and detail
                // views since LVGL scales down efficiently. We do NOT hand the thumbnail
                // cache a lifetime/alive guard: its on_success fires on a bg thread, so we
                // marshal back ourselves via tok.defer() and do the generation + lifetime
                // re-check there. An empty ctx would make fetch_for_detail_view's internal
                // ctx.is_valid() guard reject every result, so capture our lifetime token
                // (its expired() check is benign — it only short-circuits the cache's call
                // into our already-self-guarding success callback).
                ThumbnailLoadContext ctx = ThumbnailLoadContext::capture(lifetime_);

                get_thumbnail_cache().fetch_for_detail_view(
                    api_, thumbnail_rel_path, ctx,
                    [this, tok = lifetime_.token(), current_gen](const std::string& lvgl_path) {
                        // bg thread (thumbnail prescale worker): no member access here.
                        std::string path = lvgl_path;
                        tok.defer("ActivePrintMediaManager::on_thumbnail", [this, current_gen,
                                                                            path]() {
                            if (current_gen !=
                                thumbnail_load_generation_.load(std::memory_order_relaxed)) {
                                spdlog::trace("[ActivePrintMediaManager] Stale thumbnail "
                                              "callback, ignoring");
                                return;
                            }
                            printer_state_.set_print_thumbnail_path(path);
                            if (thumbnail_retry_count_ > 0) {
                                spdlog::info("[ActivePrintMediaManager] Thumbnail loaded after "
                                             "{} retries: {}",
                                             thumbnail_retry_count_, path);
                            } else {
                                spdlog::info("[ActivePrintMediaManager] Thumbnail path set: {}",
                                             path);
                            }
                            thumbnail_loaded_ = true;
                            thumbnail_retry_count_ = 0;
                            cancel_thumbnail_retry();
                            helix::MemoryMonitor::log_now("thumbnail_loaded", spdlog::level::debug);
                        });
                    },
                    [this, tok = lifetime_.token(), current_gen,
                     filename](const std::string& error) {
                        // bg thread (HTTP worker): copy the message, marshal
                        // ALL member access (retry bookkeeping) to main.
                        std::string message = error;
                        tok.defer("ActivePrintMediaManager::on_thumbnail_error",
                                  [this, current_gen, filename, message = std::move(message)]() {
                                      if (current_gen != thumbnail_load_generation_.load(
                                                             std::memory_order_relaxed)) {
                                          return; // superseded by a newer load
                                      }
                                      spdlog::warn("[ActivePrintMediaManager] Thumbnail download "
                                                   "failed for '{}' (attempt {}/{}): {}",
                                                   filename, thumbnail_retry_count_ + 1,
                                                   kMaxThumbnailAttempts, message);
                                      schedule_thumbnail_retry(filename);
                                  });
                    });
            });
        },
        [this, tok = lifetime_.token(), current_gen, filename,
         metadata_filename](const MoonrakerError& err) {
            // bg thread (WebSocket response router): copy the message, marshal
            // ALL member access (retry bookkeeping) to main. Happens when
            // Moonraker hasn't finished scanning a just-uploaded file
            // (OrcaSlicer upload-and-print) or on transient RPC failures.
            std::string message = err.message;
            tok.defer(
                "ActivePrintMediaManager::on_metadata_error",
                [this, current_gen, filename, metadata_filename, message = std::move(message)]() {
                    if (current_gen != thumbnail_load_generation_.load(std::memory_order_relaxed)) {
                        return; // superseded by a newer load
                    }
                    spdlog::warn("[ActivePrintMediaManager] Thumbnail metadata fetch failed "
                                 "for '{}' (attempt {}/{}): {}",
                                 metadata_filename, thumbnail_retry_count_ + 1,
                                 kMaxThumbnailAttempts, message);
                    schedule_thumbnail_retry(filename);
                });
        },
        true // silent - don't trigger RPC_ERROR event/toast
    );
}

// ============================================================================
// Bounded thumbnail retry
// ============================================================================

uint32_t ActivePrintMediaManager::retry_delay_ms(int retry_number) {
    switch (retry_number) {
    case 1:
        return 2000;
    case 2:
        return 5000;
    case 3:
        return 10000;
    case 4:
        return 20000;
    default:
        return 30000;
    }
}

void ActivePrintMediaManager::schedule_thumbnail_retry(const std::string& filename,
                                                       int max_retries) {
    // Main thread only — callers are either main-thread code or bg callbacks
    // that marshalled here via tok.defer().
    if (filename.empty() || filename != last_effective_filename_) {
        spdlog::debug("[ActivePrintMediaManager] Skipping retry for '{}' - no longer current",
                      filename);
        return;
    }
    if (retry_timer_) {
        return; // a retry is already pending for this filename
    }
    if (thumbnail_retry_count_ >= max_retries) {
        spdlog::warn("[ActivePrintMediaManager] Giving up on thumbnail for '{}' after {} attempts",
                     filename, thumbnail_retry_count_ + 1);
        return;
    }

    thumbnail_retry_count_++;
    uint32_t delay = retry_delay_ms(thumbnail_retry_count_);
    retry_filename_ = filename;
    retry_generation_ = thumbnail_load_generation_.load(std::memory_order_relaxed);
    retry_timer_.reset(lv_timer_create(retry_timer_cb, delay, this));
    lv_timer_set_repeat_count(retry_timer_.get(), 1); // one-shot

    spdlog::info("[ActivePrintMediaManager] Scheduling thumbnail retry for '{}' in {} ms "
                 "(attempt {}/{})",
                 filename, delay, thumbnail_retry_count_ + 1, max_retries + 1);
}

void ActivePrintMediaManager::cancel_thumbnail_retry() {
    // LvglTimerGuard::reset() neuters via lv_timer_cancel_safe() instead of
    // deleting — safe even when called from inside an UpdateQueue callback
    // while lv_timer_handler is iterating the timer list (#750/#751).
    retry_timer_.reset();
    retry_filename_.clear();
}

void ActivePrintMediaManager::retry_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<ActivePrintMediaManager*>(lv_timer_get_user_data(timer));
    // One-shot timer (repeat_count 1): LVGL deletes it right after this
    // callback returns, so release the guard's reference first.
    self->retry_timer_.release();
    self->on_retry_timer_fired();
}

void ActivePrintMediaManager::on_retry_timer_fired() {
    if (retry_filename_.empty() || retry_filename_ != last_effective_filename_) {
        spdlog::debug("[ActivePrintMediaManager] Stale retry (file changed), skipping");
        return;
    }
    if (retry_generation_ != thumbnail_load_generation_.load(std::memory_order_relaxed)) {
        spdlog::debug("[ActivePrintMediaManager] Stale retry (superseded load), skipping");
        return;
    }
    if (thumbnail_loaded_) {
        return; // loaded in the meantime (e.g., set_thumbnail_path)
    }
    spdlog::info("[ActivePrintMediaManager] Retrying thumbnail load for '{}' (attempt {}/{})",
                 retry_filename_, thumbnail_retry_count_ + 1, kMaxThumbnailAttempts);
    load_thumbnail_for_file(retry_filename_);
}

// ============================================================================
// Moonraker notification re-triggers
// ============================================================================

void ActivePrintMediaManager::register_moonraker_listeners() {
    if (!api_ || listener_api_ == api_) {
        return;
    }
    listener_api_ = api_;

    const std::string suffix = std::to_string(reinterpret_cast<uintptr_t>(this));
    filelist_handler_name_ = "apmm_filelist_" + suffix;
    klippy_ready_handler_name_ = "apmm_klippy_ready_" + suffix;

    // THREADING: method callbacks fire on the WebSocket background thread.
    // Parse into locals there; ALL member access happens on the main thread
    // via token.defer() (same idiom as AmsBackendAd5xIfs's klippy listener).
    // The token captured at registration time expires when the manager is
    // destroyed, so late notifications no-op.
    auto token = lifetime_.token();

    // Re-trigger when the file we're printing (re)appears in the file list —
    // covers Moonraker finishing its metadata scan after the print started.
    api_->register_method_callback(
        "notify_filelist_changed", filelist_handler_name_, [this, token](const json& msg) {
            // bg thread: defensive parse into plain strings only — no member
            // access. Moonraker can send null/missing fields (use find()+is_*).
            std::string action;
            std::string item_path;
            std::string source_path;
            const auto params_it = msg.find("params");
            if (params_it != msg.end() && params_it->is_array() && !params_it->empty()) {
                const json& p = (*params_it)[0];
                if (p.is_object()) {
                    action = json_util::safe_string(p, "action");
                    const auto item_it = p.find("item");
                    if (item_it != p.end() && item_it->is_object()) {
                        item_path = json_util::safe_string(*item_it, "path");
                    }
                    const auto src_it = p.find("source_item");
                    if (src_it != p.end() && src_it->is_object()) {
                        source_path = json_util::safe_string(*src_it, "path");
                    }
                }
            }
            token.defer("ActivePrintMediaManager::on_filelist_changed",
                        [this, action = std::move(action), item_path = std::move(item_path),
                         source_path = std::move(source_path)]() {
                            handle_filelist_changed(action, item_path, source_path);
                        });
        });

    // Re-trigger on klippy ready — covers WebSocket reconnect mid-print where
    // the filename subject never changes, so no observer fires.
    api_->register_method_callback(
        "notify_klippy_ready", klippy_ready_handler_name_, [this, token](const json& /*msg*/) {
            token.defer("ActivePrintMediaManager::on_klippy_ready",
                        [this]() { retrigger_thumbnail_load("klippy_ready"); });
        });

    spdlog::debug("[ActivePrintMediaManager] Registered Moonraker notification listeners");
}

void ActivePrintMediaManager::unregister_moonraker_listeners() {
    // NOTE: during teardown this is a no-op BY DESIGN. Both teardown paths
    // (Application::shutdown and the soft-restart path in application.cpp)
    // call set_moonraker_manager(nullptr) at step 1, long before
    // deinit_active_print_media_manager() runs — so get_moonraker_manager()
    // is always null here during teardown and the unregistration is skipped.
    // The branch below only executes on a set_api() transition (api swapped
    // or cleared while the app is running).
    //
    // Safety for the skipped case does NOT depend on unregistration: the
    // registered lambdas capture a lifetime token that expires when this
    // manager is destroyed (stale notifications parse into locals bg-side and
    // the token.defer() apply no-ops), and the client owning the callback map
    // is destroyed later in the same teardown sequence.
    auto* mgr = get_moonraker_manager();
    if (mgr && listener_api_) {
        if (!filelist_handler_name_.empty()) {
            listener_api_->unregister_method_callback("notify_filelist_changed",
                                                      filelist_handler_name_);
        }
        if (!klippy_ready_handler_name_.empty()) {
            listener_api_->unregister_method_callback("notify_klippy_ready",
                                                      klippy_ready_handler_name_);
        }
    }
    listener_api_ = nullptr;
    filelist_handler_name_.clear();
    klippy_ready_handler_name_.clear();
}

void ActivePrintMediaManager::handle_filelist_changed(const std::string& action,
                                                      const std::string& item_path,
                                                      const std::string& source_path) {
    // Main thread (marshalled via token.defer).
    if (thumbnail_loaded_ || last_effective_filename_.empty()) {
        return;
    }

    // Only actions that can make metadata/thumbnails (re)appear. Notably NOT
    // delete_file — re-querying a just-deleted file would start a
    // guaranteed-to-fail retry ladder.
    if (action != "create_file" && action != "modify_file" && action != "move_file" &&
        action != "root_update") {
        return;
    }

    if (action == "root_update") {
        spdlog::info("[ActivePrintMediaManager] filelist root_update with thumbnail missing");
        retrigger_thumbnail_load("filelist_changed");
        return;
    }

    // Metadata lookups use the resolved filename, so match against both forms.
    const std::string metadata_filename = resolve_gcode_filename(last_effective_filename_);
    const bool item_matches = !item_path.empty() && (item_path == metadata_filename ||
                                                     item_path == last_effective_filename_);
    const bool source_matches = !source_path.empty() && (source_path == metadata_filename ||
                                                         source_path == last_effective_filename_);

    if (action == "move_file" && !item_matches && source_matches) {
        // The printing file was moved/renamed: its metadata now lives under
        // the destination path (item). Reload from there — but only if the
        // notification actually carried a destination.
        if (item_path.empty()) {
            return;
        }
        spdlog::info("[ActivePrintMediaManager] current print '{}' moved to '{}' - reloading "
                     "thumbnail from destination",
                     metadata_filename, item_path);
        thumbnail_retry_count_ = 0;
        cancel_thumbnail_retry();
        load_thumbnail_for_file(item_path);
        return;
    }

    if (!item_matches && !source_matches) {
        return;
    }
    spdlog::info("[ActivePrintMediaManager] filelist change ({}) matches current print '{}'",
                 action, metadata_filename);
    retrigger_thumbnail_load("filelist_changed");
}

void ActivePrintMediaManager::retrigger_thumbnail_load(const char* reason) {
    // Main thread (marshalled via token.defer).
    if (thumbnail_loaded_ || last_effective_filename_.empty()) {
        return;
    }
    spdlog::info("[ActivePrintMediaManager] {} - reloading thumbnail for '{}'", reason,
                 last_effective_filename_);
    thumbnail_retry_count_ = 0;
    cancel_thumbnail_retry();
    load_thumbnail_for_file(last_effective_filename_);
}

void ActivePrintMediaManager::clear_print_info() {
    thumbnail_source_filename_.clear();
    last_effective_filename_.clear();
    last_loaded_thumbnail_filename_.clear();
    cancel_thumbnail_retry();
    thumbnail_retry_count_ = 0;
    thumbnail_loaded_ = false;

    // Thread-safe clear of shared subjects (capture printer_state_ for testability)
    PrinterState* state = &printer_state_;
    helix::ui::queue_update([state]() {
        state->set_print_thumbnail_path("");
        state->set_print_display_filename("");
        spdlog::debug("[ActivePrintMediaManager] Cleared print info subjects");
    });
}

} // namespace helix

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_modal.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_split_button.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "color_utils.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "format_utils.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#endif
#include "ui_overlay_qr_scanner.h"
#include "ui_toast_manager.h"

#include "moonraker_api.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace helix::ui {

// Static member initialization
bool AmsEditModal::callbacks_registered_ = false;

// Fire-and-forget: notify Moonraker of the active spool so other clients
// (Mainsail, Fluidd) see the change and filament tracking works.
// Pass 0 to clear the active spool (unlink).
static void sync_active_spool(MoonrakerAPI* api, int spool_id) {
    spdlog::info("[AmsEditModal] Syncing active spool to {} on server", spool_id);
    api->spoolman().set_active_spool(
        spool_id,
        [spool_id]() {
            spdlog::debug("[AmsEditModal] Active spool synced to {} on server", spool_id);
        },
        [spool_id](const MoonrakerError& err) {
            spdlog::warn("[AmsEditModal] Failed to sync active spool to {}: {}", spool_id,
                         err.message);
        });
}

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsEditModal::AmsEditModal() {
    spdlog::debug("[AmsEditModal] Constructed");
}

AmsEditModal::~AmsEditModal() {
    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();

    // Modal destructor will call hide() if visible
    spdlog::trace("[AmsEditModal] Destroyed");
}

AmsEditModal::AmsEditModal(AmsEditModal&& other) noexcept
    : Modal(std::move(other)), slot_index_(other.slot_index_),
      original_info_(std::move(other.original_info_)),
      working_info_(std::move(other.working_info_)), api_(other.api_),
      completion_callback_(std::move(other.completion_callback_)),
      remaining_pre_edit_pct_(other.remaining_pre_edit_pct_),
      color_picker_(std::move(other.color_picker_)),
      subjects_initialized_(other.subjects_initialized_),
      cached_spools_(std::move(other.cached_spools_)) {
    // Copy buffers
    std::memcpy(slot_indicator_buf_, other.slot_indicator_buf_, sizeof(slot_indicator_buf_));
    std::memcpy(color_name_buf_, other.color_name_buf_, sizeof(color_name_buf_));
    std::memcpy(temp_nozzle_buf_, other.temp_nozzle_buf_, sizeof(temp_nozzle_buf_));
    std::memcpy(temp_bed_buf_, other.temp_bed_buf_, sizeof(temp_bed_buf_));
    std::memcpy(remaining_pct_buf_, other.remaining_pct_buf_, sizeof(remaining_pct_buf_));

    // Subjects are not movable - they stay with original
    other.subjects_initialized_ = false;
    other.api_ = nullptr;
    other.slot_index_ = -1;
}

AmsEditModal& AmsEditModal::operator=(AmsEditModal&& other) noexcept {
    if (this != &other) {
        Modal::operator=(std::move(other));
        slot_index_ = other.slot_index_;
        original_info_ = std::move(other.original_info_);
        working_info_ = std::move(other.working_info_);
        api_ = other.api_;
        completion_callback_ = std::move(other.completion_callback_);
        remaining_pre_edit_pct_ = other.remaining_pre_edit_pct_;
        color_picker_ = std::move(other.color_picker_);
        subjects_initialized_ = other.subjects_initialized_;
        cached_spools_ = std::move(other.cached_spools_);
        std::memcpy(slot_indicator_buf_, other.slot_indicator_buf_, sizeof(slot_indicator_buf_));
        std::memcpy(color_name_buf_, other.color_name_buf_, sizeof(color_name_buf_));
        std::memcpy(temp_nozzle_buf_, other.temp_nozzle_buf_, sizeof(temp_nozzle_buf_));
        std::memcpy(temp_bed_buf_, other.temp_bed_buf_, sizeof(temp_bed_buf_));
        std::memcpy(remaining_pct_buf_, other.remaining_pct_buf_, sizeof(remaining_pct_buf_));
        other.subjects_initialized_ = false;
        other.api_ = nullptr;
        other.slot_index_ = -1;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void AmsEditModal::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

bool AmsEditModal::show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                                 MoonrakerAPI* api) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Initialize subjects if needed
    init_subjects();

    // Store state
    slot_index_ = slot_index;
    original_info_ = initial_info;
    working_info_ = initial_info;
    filament_user_edited_ = false; // no user edit yet this session (#1071)
    api_ = api ? api : get_moonraker_api();
    remaining_pre_edit_pct_ = 0;
    cached_spools_.clear();
    vendors_loaded_ = false;

    // Reset remaining mode subject before showing (0 = view mode)
    lv_subject_set_int(&remaining_mode_subject_, 0);

    // Set static active instance BEFORE Modal::show() so callbacks during
    // on_show() (e.g., async fetch triggers) can resolve the instance
    s_active_instance_ = this;

    // Show the modal via Modal
    if (!Modal::show(parent)) {
        s_active_instance_ = nullptr;
        return false;
    }

    // Always start on the form view — it's the primary interface showing current
    // slot state. The Spoolman picker is accessible via "Choose Spool" button.
    switch_to_form();

    // If linked to Spoolman, fetch authoritative filament data (vendor, material, color)
    // so the form shows current Spoolman state, not stale backend data
    if (working_info_.spoolman_id > 0 && api_) {
        const int spool_id = working_info_.spoolman_id;
        auto token = lifetime_.token();
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                if (!spool || token.expired())
                    return;
                // Capture Spoolman's authoritative data for the spool
                int fetched_filament_id = spool->filament_id;
                int fetched_vendor_id = spool->vendor_id;
                std::string fetched_vendor = spool->vendor;
                std::string fetched_material = spool->material;
                std::string fetched_color_hex = spool->color_hex;
                token.defer([this, spool_id, fetched_filament_id, fetched_vendor_id,
                             fetched_vendor = std::move(fetched_vendor),
                             fetched_material = std::move(fetched_material),
                             fetched_color_hex = std::move(fetched_color_hex)]() {
                    if (fetched_filament_id > 0) {
                        original_info_.spoolman_filament_id = fetched_filament_id;
                        working_info_.spoolman_filament_id = fetched_filament_id;
                    }
                    if (fetched_vendor_id > 0) {
                        original_info_.spoolman_vendor_id = fetched_vendor_id;
                        working_info_.spoolman_vendor_id = fetched_vendor_id;
                    }
                    // Update brand/material from Spoolman (authoritative source)
                    if (!fetched_vendor.empty() && working_info_.brand != fetched_vendor) {
                        spdlog::debug(
                            "[AmsEditModal] Updating vendor from '{}' to '{}' (Spoolman spool {})",
                            working_info_.brand, fetched_vendor, spool_id);
                        original_info_.brand = fetched_vendor;
                        working_info_.brand = fetched_vendor;
                    }
                    if (!fetched_material.empty() && working_info_.material != fetched_material) {
                        spdlog::debug("[AmsEditModal] Updating material from '{}' to '{}' "
                                      "(Spoolman spool {})",
                                      working_info_.material, fetched_material, spool_id);
                        original_info_.material = fetched_material;
                        working_info_.material = fetched_material;
                    }
                    if (!fetched_color_hex.empty()) {
                        uint32_t rgb = 0;
                        if (helix::parse_hex_color(fetched_color_hex.c_str(), rgb) &&
                            working_info_.color_rgb != rgb) {
                            spdlog::debug("[AmsEditModal] Updating color from {:#08x} to {:#08x} "
                                          "(Spoolman spool {})",
                                          working_info_.color_rgb, rgb, spool_id);
                            original_info_.color_rgb = rgb;
                            working_info_.color_rgb = rgb;
                        }
                    }
                    spdlog::debug("[AmsEditModal] Synced spool {} from Spoolman: vendor='{}', "
                                  "material='{}', filament_id={}, vendor_id={}",
                                  spool_id, working_info_.brand, working_info_.material,
                                  fetched_filament_id, fetched_vendor_id);
                    update_ui();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditModal] Failed to fetch spool {}: {}", spool_id, err.message);
            });
    }

    spdlog::info("[AmsEditModal] Shown for slot {} (spoolman_id={}, brand={}, material={})",
                 slot_index, initial_info.spoolman_id, initial_info.brand, initial_info.material);
    return true;
}

// ============================================================================
// Modal Hooks
// ============================================================================

void AmsEditModal::on_show() {
    // Re-set active instance here — if Modal::show() had to hide() a previous
    // instance first, on_hide() would have cleared s_active_instance_
    s_active_instance_ = this;

    // Fetch vendor list from Spoolman (async, will update dropdown when ready)
    fetch_vendors_from_spoolman();

    // Bind labels to subjects for reactive text updates (save observers for cleanup)
    lv_obj_t* slot_indicator = find_widget("slot_indicator");
    if (slot_indicator) {
        slot_indicator_observer_ =
            lv_label_bind_text(slot_indicator, &slot_indicator_subject_, nullptr);
    }

    lv_obj_t* color_name_label = find_widget("color_name_label");
    if (color_name_label) {
        color_name_observer_ = lv_label_bind_text(color_name_label, &color_name_subject_, nullptr);
    }

    lv_obj_t* spool_name_label = find_widget("spool_name_label");
    if (spool_name_label) {
        spool_name_observer_ = lv_label_bind_text(spool_name_label, &spool_name_subject_, nullptr);
    }

    lv_obj_t* temp_nozzle_label = find_widget("temp_nozzle_label");
    if (temp_nozzle_label) {
        temp_nozzle_observer_ =
            lv_label_bind_text(temp_nozzle_label, &temp_nozzle_subject_, nullptr);
    }

    lv_obj_t* temp_bed_label = find_widget("temp_bed_label");
    if (temp_bed_label) {
        temp_bed_observer_ = lv_label_bind_text(temp_bed_label, &temp_bed_subject_, nullptr);
    }

    lv_obj_t* remaining_pct_label = find_widget("remaining_pct_label");
    if (remaining_pct_label) {
        remaining_pct_observer_ =
            lv_label_bind_text(remaining_pct_label, &remaining_pct_subject_, nullptr);
    }

    lv_obj_t* save_btn_label = find_widget("btn_save_label");
    if (save_btn_label) {
        save_btn_text_observer_ =
            lv_label_bind_text(save_btn_label, &save_btn_text_subject_, nullptr);
    }

    // Update the modal UI with current slot data
    update_ui();

    // Set initial sync button state (disabled since nothing is dirty yet)
    update_sync_button_state();

    // Set initial Spoolman button state
    update_spoolman_button_state();
}

void AmsEditModal::on_hide() {
    // Only clear active instance if WE are the active one — a deferred on_hide()
    // from a previous show/hide cycle must not clear a freshly-set instance
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    // Check if LVGL is initialized - may be called from destructor during static destruction
    if (!lv_is_initialized()) {
        return;
    }

    // Observer cleanup is handled by SubjectManager::deinit_all() in deinit_subjects()
    // which calls lv_subject_deinit() on each subject. This properly removes all
    // attached observers from the subject side. We avoid manual lv_observer_remove()
    // because the destructor calls deinit_subjects() before the Modal base destructor
    // calls on_hide(), which would leave us with stale observer pointers.

    // Reset edit mode subject
    if (subjects_initialized_) {
        lv_subject_set_int(&remaining_mode_subject_, 0);
        lv_subject_set_int(&view_mode_subject_, 0);
        lv_subject_set_int(&picker_state_subject_, 0);
    }

    // Clear cached picker data
    cached_spools_.clear();

    spdlog::debug("[AmsEditModal] on_hide()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize string subjects with empty/default buffers (local binding only, not XML
    // registered)
    slot_indicator_buf_[0] = '-';
    slot_indicator_buf_[1] = '-';
    slot_indicator_buf_[2] = '\0';
    color_name_buf_[0] = '\0';
    spool_name_buf_[0] = '\0';
    snprintf(temp_nozzle_buf_, sizeof(temp_nozzle_buf_), "200-230°C");
    snprintf(temp_bed_buf_, sizeof(temp_bed_buf_), "60°C");
    snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "100%%");

    lv_subject_init_string(&slot_indicator_subject_, slot_indicator_buf_, nullptr,
                           sizeof(slot_indicator_buf_), "--");
    subjects_.register_subject(&slot_indicator_subject_);

    lv_subject_init_string(&color_name_subject_, color_name_buf_, nullptr, sizeof(color_name_buf_),
                           "");
    subjects_.register_subject(&color_name_subject_);

    lv_subject_init_string(&spool_name_subject_, spool_name_buf_, nullptr, sizeof(spool_name_buf_),
                           "");
    subjects_.register_subject(&spool_name_subject_);

    lv_subject_init_string(&temp_nozzle_subject_, temp_nozzle_buf_, nullptr,
                           sizeof(temp_nozzle_buf_), "200-230°C");
    subjects_.register_subject(&temp_nozzle_subject_);

    lv_subject_init_string(&temp_bed_subject_, temp_bed_buf_, nullptr, sizeof(temp_bed_buf_),
                           "60°C");
    subjects_.register_subject(&temp_bed_subject_);

    lv_subject_init_string(&remaining_pct_subject_, remaining_pct_buf_, nullptr,
                           sizeof(remaining_pct_buf_), "100%");
    subjects_.register_subject(&remaining_pct_subject_);

    // Initialize save button text subject
    snprintf(save_btn_text_buf_, sizeof(save_btn_text_buf_), "%s", lv_tr("Close"));
    lv_subject_init_string(&save_btn_text_subject_, save_btn_text_buf_, nullptr,
                           sizeof(save_btn_text_buf_), lv_tr("Close"));
    subjects_.register_subject(&save_btn_text_subject_);

    // Initialize remaining mode subject (0=view, 1=edit) - registered globally for XML binding
    UI_MANAGED_SUBJECT_INT(remaining_mode_subject_, 0, "edit_remaining_mode", subjects_);

    // Initialize view mode subject (0=form, 1=picker) - registered globally for XML binding
    UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "edit_modal_view", subjects_);

    // Initialize picker state subject (0=loading, 1=empty, 2=content) - registered globally
    UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[AmsEditModal] Subjects initialized");
}

void AmsEditModal::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // SubjectManager handles all lv_subject_deinit() calls via RAII
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[AmsEditModal] Subjects deinitialized");
}

void AmsEditModal::fetch_vendors_from_spoolman() {
    // Resolve API: prefer stored api_, fall back to global
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!api_ || vendors_loaded_) {
        return;
    }

    // Skip Spoolman API call if not configured (avoids "method not found" toast)
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (spoolman_subj && lv_subject_get_int(spoolman_subj) != 1) {
        return;
    }

    auto token = lifetime_.token();

    // Use dedicated vendor endpoint instead of downloading all spools
    api_->spoolman().get_spoolman_vendors(
        [this, token](const std::vector<VendorInfo>& vendors_result) {
            if (token.expired())
                return;
            // Build vendor list on background thread, then marshal to main
            std::set<std::string> unique_vendors;
            unique_vendors.insert("Generic"); // Always have Generic as first option
            for (const auto& vendor : vendors_result) {
                if (!vendor.name.empty()) {
                    unique_vendors.insert(vendor.name);
                }
            }

            // Build vendor list with IDs and options string (local copies, no member access)
            // Build a name→id map for lookup
            std::map<std::string, int> vendor_id_map;
            for (const auto& vendor : vendors_result) {
                if (!vendor.name.empty()) {
                    vendor_id_map[vendor.name] = vendor.id;
                }
            }

            std::vector<VendorInfo> vendors;
            std::string options;
            for (const auto& name : unique_vendors) {
                if (!options.empty()) {
                    options += '\n';
                }
                options += name;
                VendorInfo vi;
                vi.name = name;
                auto it = vendor_id_map.find(name);
                vi.id = (it != vendor_id_map.end()) ? it->second : 0;
                vendors.push_back(std::move(vi));
            }

            // Marshal member writes to main thread
            token.defer(
                [this, vendors = std::move(vendors), options = std::move(options)]() mutable {
                    vendor_list_ = std::move(vendors);
                    vendor_options_ = std::move(options);
                    vendors_loaded_ = true;
                    spdlog::debug("[AmsEditModal] Loaded {} vendors from Spoolman",
                                  vendor_list_.size());
                    update_vendor_dropdown();
                });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AmsEditModal] Failed to fetch Spoolman vendors: {}", err.message);
            // Keep using fallback vendors
        });
}

void AmsEditModal::update_vendor_dropdown() {
    if (!dialog_ || vendor_options_.empty()) {
        return;
    }

    lv_obj_t* vendor_dropdown = find_widget("vendor_dropdown");
    if (!vendor_dropdown) {
        return;
    }

    lv_dropdown_set_options(vendor_dropdown, vendor_options_.c_str());

    // Set selection based on working_info_.brand, and populate vendor_id if missing.
    // Default to "Generic" (not necessarily index 0 since the list is alphabetical).
    int vendor_idx = -1;
    int generic_idx = 0;
    for (size_t i = 0; i < vendor_list_.size(); i++) {
        if (vendor_list_[i].name == "Generic") {
            generic_idx = static_cast<int>(i);
        }
        if (!working_info_.brand.empty() && working_info_.brand == vendor_list_[i].name) {
            vendor_idx = static_cast<int>(i);
            if (working_info_.spoolman_vendor_id == 0 && vendor_list_[i].id > 0) {
                working_info_.spoolman_vendor_id = vendor_list_[i].id;
                spdlog::debug("[AmsEditModal] Resolved vendor_id={} from vendor list for '{}'",
                              vendor_list_[i].id, vendor_list_[i].name);
            }
            break;
        }
    }
    if (vendor_idx < 0) {
        vendor_idx = generic_idx;
        if (working_info_.brand.empty())
            working_info_.brand = "Generic";
        if (original_info_.brand.empty())
            original_info_.brand = "Generic";
    }
    lv_dropdown_set_selected(vendor_dropdown, vendor_idx);
}

// ============================================================================
// View Switching
// ============================================================================

void AmsEditModal::switch_to_picker() {
    if (!subjects_initialized_) {
        spdlog::warn("[AmsEditModal] switch_to_picker() aborted: subjects not initialized");
        return;
    }
    spdlog::debug("[AmsEditModal] Switching to picker view (dialog_={}, api_={})",
                  static_cast<void*>(dialog_), static_cast<void*>(api_));
    lv_subject_set_int(&view_mode_subject_, 1);
    populate_picker();
}

void AmsEditModal::switch_to_form() {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&view_mode_subject_, 0);
    spdlog::debug("[AmsEditModal] Switched to form view");
}

void AmsEditModal::populate_picker() {
    // Resolve API: prefer stored api_, fall back to global (matches SpoolmanPanel pattern)
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!dialog_ || !api_) {
        spdlog::warn("[AmsEditModal] populate_picker() aborted: dialog_={}, api_={}",
                     static_cast<void*>(dialog_), static_cast<void*>(api_));
        lv_subject_set_int(&picker_state_subject_, 1);
        return;
    }

    // Show loading state
    lv_subject_set_int(&picker_state_subject_, 0);

    // Clear search input
    lv_obj_t* search = find_widget("picker_search");
    if (search) {
        lv_textarea_set_text(search, "");
    }

    auto token = lifetime_.token();

    spdlog::debug("[AmsEditModal] populate_picker() fetching spools from Spoolman...");

    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            if (token.expired())
                return;
            spdlog::debug("[AmsEditModal] Spoolman returned {} spools", spools.size());
            token.defer([this, spools]() {
                if (!dialog_) {
                    spdlog::warn("[AmsEditModal] populate_picker callback dropped: dialog_ null");
                    return;
                }
                if (!subjects_initialized_) {
                    spdlog::warn("[AmsEditModal] populate_picker callback dropped: subjects not "
                                 "initialized");
                    return;
                }

                if (spools.empty()) {
                    spdlog::debug("[AmsEditModal] Spoolman returned empty spool list");
                    lv_subject_set_int(&picker_state_subject_, 1);
                    return;
                }

                cached_spools_ = spools;
                render_spool_list("");
            });
        },
        [this, token](const MoonrakerError& err) {
            spdlog::warn("[AmsEditModal] Spoolman fetch error: {}", err.message);
            token.defer([this, msg = err.message]() {
                if (!dialog_ || !subjects_initialized_) {
                    spdlog::warn("[AmsEditModal] Error callback dropped: dialog_={}, "
                                 "subjects={}",
                                 static_cast<void*>(dialog_), subjects_initialized_);
                    return;
                }
                spdlog::warn("[AmsEditModal] Failed to fetch spools: {}", msg);
                lv_subject_set_int(&picker_state_subject_, 1);
            });
        });
}

void AmsEditModal::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = find_widget("picker_spool_list");
    if (!spool_list) {
        return;
    }

    // Invoked from a token.defer() callback (UpdateQueue batch). Sync
    // lv_obj_clean in that context corrupts LVGL's event linked list →
    // SIGSEGV in lv_event_mark_deleted (#776).
    helix::ui::safe_clean_children(spool_list);

    // Reuse shared filter_spools() from spoolman_types
    auto filtered = filter_spools(cached_spools_, filter);

    // Get spool IDs assigned to other tools (exclude current slot's tool)
    auto in_use = ToolState::instance().assigned_spool_ids(slot_index_);

    for (const auto& spool : filtered) {
        lv_obj_t* item =
            static_cast<lv_obj_t*>(lv_xml_create(spool_list, "spoolman_spool_item", nullptr));
        if (!item) {
            continue;
        }

        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(spool.id)));

        lv_obj_t* name_label = lv_obj_find_by_name(item, "spool_name");
        if (name_label) {
            std::string name = "#" + std::to_string(spool.id) + " ";
            name += spool.vendor.empty() ? spool.material : (spool.vendor + " " + spool.material);
            lv_label_set_text(name_label, name.c_str());
        }

        lv_obj_t* color_label = lv_obj_find_by_name(item, "spool_color");
        if (color_label && !spool.color_name.empty()) {
            lv_label_set_text(color_label, spool.color_name.c_str());
        }

        lv_obj_t* weight_label = lv_obj_find_by_name(item, "spool_weight");
        if (weight_label && spool.remaining_weight_g > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fg", spool.remaining_weight_g);
            lv_label_set_text(weight_label, buf);
        }

        lv_obj_t* swatch = lv_obj_find_by_name(item, "spool_swatch");
        if (swatch && !spool.color_hex.empty()) {
            const char* hex = spool.color_hex.c_str();
            if (hex[0] == '#') {
                hex++;
            }
            uint32_t hex_val = static_cast<uint32_t>(strtoul(hex, nullptr, 16));
            lv_color_t color = lv_color_hex(hex_val);
            lv_obj_set_style_bg_color(swatch, color, 0);
            lv_obj_set_style_border_color(swatch, color, 0);
        }

        // Mark current spool as checked (matching spoolman list view pattern)
        bool is_selected = (spool.id == working_info_.spoolman_id);
        lv_obj_set_state(item, LV_STATE_CHECKED, is_selected);
        if (is_selected) {
            lv_obj_t* check_icon = lv_obj_find_by_name(item, "selected_icon");
            if (check_icon) {
                lv_obj_remove_flag(check_icon, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Disable spools already assigned to other tools
        if (in_use.count(spool.id)) {
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_subject_set_int(&picker_state_subject_, filtered.empty() ? 1 : 2);
    spdlog::debug("[AmsEditModal] Rendered {} spool items (filter='{}')", filtered.size(), filter);
}

void AmsEditModal::handle_spool_selected(int spool_id) {
    spdlog::info("[AmsEditModal] Spool {} selected for slot {}", spool_id, slot_index_);

    // Look up SpoolInfo from cached spools
    for (const auto& spool : cached_spools_) {
        if (spool.id == spool_id) {
            // Auto-fill working_info_ from the selected spool
            working_info_.spoolman_id = spool.id;
            working_info_.spoolman_filament_id = spool.filament_id;
            working_info_.spoolman_vendor_id = spool.vendor_id;
            working_info_.color_name = spool.color_name;
            working_info_.material = spool.material;
            working_info_.brand = spool.vendor;
            working_info_.spool_name = spool.vendor + " " + spool.material;
            working_info_.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
            working_info_.total_weight_g = static_cast<float>(spool.initial_weight_g);
            working_info_.nozzle_temp_min = spool.nozzle_temp_min;
            working_info_.nozzle_temp_max = spool.nozzle_temp_max;
            working_info_.bed_temp = spool.bed_temp_recommended;

            // Parse color hex to RGB
            if (!spool.color_hex.empty()) {
                uint32_t rgb = 0;
                if (helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
                    working_info_.color_rgb = rgb;
                } else {
                    spdlog::warn("[AmsEditModal] Failed to parse color hex: {}", spool.color_hex);
                }
            }

            break;
        }
    }

    // Switch to form view and refresh UI
    switch_to_form();
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditModal::handle_manual_entry() {
    spdlog::debug("[AmsEditModal] Manual entry requested - switching to form");
    switch_to_form();
}

void AmsEditModal::handle_change_spool() {
    spdlog::debug("[AmsEditModal] Change spool requested - switching to picker");
    switch_to_picker();
}

void AmsEditModal::handle_picker_search(const char* text) {
    if (cached_spools_.empty()) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditModal::handle_unlink() {
    spdlog::info("[AmsEditModal] Unlink requested for slot {}", slot_index_);
    working_info_.spoolman_id = 0;
    working_info_.spool_name.clear();
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditModal::handle_spool_details() {
    if (working_info_.spoolman_id <= 0 || !api_) {
        return;
    }

    // Find the SpoolInfo in our cache, or build a minimal one
    SpoolInfo spool_info;
    bool found = false;
    for (const auto& spool : cached_spools_) {
        if (spool.id == working_info_.spoolman_id) {
            spool_info = spool;
            found = true;
            break;
        }
    }

    if (!found) {
        spool_info.id = working_info_.spoolman_id;
        spool_info.filament_id = working_info_.spoolman_filament_id;
        spool_info.vendor = working_info_.brand;
        spool_info.material = working_info_.material;
        spool_info.color_name = working_info_.color_name;
        spool_info.remaining_weight_g = working_info_.remaining_weight_g;
        spool_info.initial_weight_g = working_info_.total_weight_g;
        if (working_info_.color_rgb != 0) {
            char hex_buf[8];
            snprintf(hex_buf, sizeof(hex_buf), "#%06X", working_info_.color_rgb);
            spool_info.color_hex = hex_buf;
        }
    }

    // When spool details are saved, re-fetch the spool data to update our form
    auto token = lifetime_.token();
    spool_edit_modal_.set_completion_callback([this, token](bool saved) {
        if (!saved || token.expired() || !api_) {
            return;
        }
        // Re-fetch the spool to pick up any changes (color, weight, etc.)
        int spool_id = working_info_.spoolman_id;
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                if (token.expired() || !spool.has_value()) {
                    return;
                }
                token.defer([this, spool = *spool]() {
                    // Update working_info_ from refreshed spool data
                    working_info_.brand = spool.vendor;
                    working_info_.material = spool.material;
                    working_info_.color_name = spool.color_name;
                    if (!spool.color_hex.empty()) {
                        uint32_t rgb = 0;
                        if (helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
                            working_info_.color_rgb = rgb;
                        }
                    }
                    if (spool.remaining_weight_g > 0 && spool.initial_weight_g > 0) {
                        working_info_.remaining_weight_g =
                            static_cast<float>(spool.remaining_weight_g);
                        working_info_.total_weight_g = static_cast<float>(spool.initial_weight_g);
                    }
                    update_ui();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditModal] Failed to refresh spool {} after edit: {}", spool_id,
                             err.message);
            });
    });

    lv_obj_t* parent = dialog_ ? lv_obj_get_parent(dialog_) : lv_screen_active();
    spool_edit_modal_.show_for_spool(parent, spool_info, api_);
}

void AmsEditModal::handle_scan_qr() {
    spdlog::info("[AmsEditModal] Scan QR requested for slot {}", slot_index_);

    int slot = slot_index_;
    auto* api = api_ ? api_ : get_moonraker_api();
    auto& scanner = helix::ui::get_qr_scanner_overlay();
    scanner.show(lv_obj_get_screen(dialog()), slot, [slot, api](const SpoolInfo& spool) {
        // QR scan result: apply spool data directly.
        // The modal and QR overlay are both closing, so we can't populate
        // the form — instead save the data immediately.
        SlotInfo info;
        info.slot_index = slot;
        info.global_index = slot;
        info.spoolman_id = spool.id;
        info.spoolman_filament_id = spool.filament_id;
        info.spoolman_vendor_id = spool.vendor_id;
        info.color_name = spool.color_name;
        info.material = spool.material;
        info.brand = spool.vendor;
        info.spool_name = spool.vendor + " " + spool.material;
        info.remaining_weight_g = static_cast<float>(spool.remaining_weight_g);
        info.total_weight_g = static_cast<float>(spool.initial_weight_g);
        info.nozzle_temp_min = spool.nozzle_temp_min;
        info.nozzle_temp_max = spool.nozzle_temp_max;
        info.bed_temp = spool.bed_temp_recommended;
        if (!spool.color_hex.empty()) {
            uint32_t rgb = 0;
            if (helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
                info.color_rgb = rgb;
            }
        }

        if (slot == -2) {
            // External spool
            AmsState::instance().set_external_spool_info(info);
            spdlog::info("[AmsEditModal] QR scan auto-saved spool #{} to external spool", spool.id);
        } else {
            // AMS slot
            AmsBackend* be = AmsState::instance().get_backend();
            if (be) {
                AmsError err = be->set_slot_info(slot, info);
                if (err.success()) {
                    AmsState::instance().sync_from_backend();
                    spdlog::info("[AmsEditModal] QR scan auto-saved spool #{} to slot {}", spool.id,
                                 slot);
                } else {
                    spdlog::error("[AmsEditModal] QR scan save failed: {}", err.user_msg);
                }
            }
        }

        // Sync active spool with Moonraker
        if (api && spool.id > 0) {
            sync_active_spool(api, spool.id);
        }

        NOTIFY_INFO("{} {} assigned via QR scan", spool.vendor, spool.material);
    });

    // Close the modal — the QR scanner will handle everything
    hide();
}

#if HELIX_HAS_LABEL_PRINTER
void AmsEditModal::handle_print_label() {
    auto& settings = helix::LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

    // Build SpoolInfo from AMS slot data
    SpoolInfo spool_info;
    bool found = false;
    for (const auto& spool : cached_spools_) {
        if (spool.id == working_info_.spoolman_id) {
            spool_info = spool;
            found = true;
            break;
        }
    }

    if (!found) {
        spool_info.id = working_info_.spoolman_id;
        spool_info.vendor = working_info_.brand;
        spool_info.material = working_info_.material;
        spool_info.color_name = working_info_.color_name;
        spool_info.remaining_weight_g = working_info_.remaining_weight_g;
        spool_info.initial_weight_g = working_info_.total_weight_g;
    }

    // Use the standard print flow (handles all printer types including IPP modal)
    auto print_cb = [](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Label printed"), 2000);
        } else {
            spdlog::error("[AmsEditModal] Print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          helix::friendly_label_printer_error(error).c_str(), 5000);
        }
    };

    if (!helix::maybe_show_ipp_print_modal(spool_info, print_cb)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing label..."), 2000);
        helix::print_spool_label(spool_info, print_cb);
    }
}
#endif

void AmsEditModal::update_spoolman_button_state() {
    if (!dialog_) {
        return;
    }

    // Ensure the spoolman_actions container visibility matches the current subject value.
    // The XML bind_flag_if_eq fires asynchronously, so when the modal opens before Spoolman
    // detection completes, the container stays hidden even after the subject becomes 1.
    // Reading the subject synchronously here closes that race window (#311).
    lv_obj_t* actions_container = find_widget("spoolman_actions");
    if (actions_container) {
        auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
        if (has_spoolman) {
            lv_obj_remove_flag(actions_container, LV_OBJ_FLAG_HIDDEN);
            // Retry vendor fetch if it was skipped due to the race (#311)
            if (!vendors_loaded_) {
                fetch_vendors_from_spoolman();
            }
        } else {
            lv_obj_add_flag(actions_container, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_t* btn_actions = find_widget("btn_spool_actions");
    lv_obj_t* btn_scan_qr = find_widget("btn_scan_qr_code");

    if (working_info_.spoolman_id > 0) {
        // Linked: show split button with all spool actions
        if (btn_scan_qr) {
            lv_obj_add_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
        }
        if (btn_actions) {
            lv_obj_remove_flag(btn_actions, LV_OBJ_FLAG_HIDDEN);
            ui_split_button_set_text(btn_actions, lv_tr("Scan QR Code"));

            // Build options list with translated strings
            std::string options = std::string(lv_tr("Scan QR Code")) + "\n" +
                                  lv_tr("Spool Details") + "\n" + lv_tr("Unlink");
#if HELIX_HAS_LABEL_PRINTER
            if (helix::LabelPrinterSettingsManager::instance().is_configured()) {
                options += std::string("\n") + lv_tr("Print Label");
            }
#endif
            ui_split_button_set_options(btn_actions, options.c_str());
        }
    } else {
        // Not linked: show standalone "Scan QR Code" button, hide split button
        if (btn_scan_qr) {
            lv_obj_remove_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
        }
        if (btn_actions) {
            lv_obj_add_flag(btn_actions, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void AmsEditModal::update_ui() {
    if (!dialog_) {
        return;
    }

    // Update slot indicator via subject (used in header)
    if (slot_index_ < 0) {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), "%s",
                 lv_tr("External Filament"));
    } else {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), lv_tr("Slot %d Filament"),
                 slot_index_ + 1);
    }
    lv_subject_copy_string(&slot_indicator_subject_, slot_indicator_buf_);

    // Update Spoolman ID label in header
    lv_obj_t* spoolman_label = find_widget("spoolman_id_label");
    if (spoolman_label) {
        if (working_info_.spoolman_id > 0) {
            char spoolman_text[32];
            snprintf(spoolman_text, sizeof(spoolman_text), "(Spoolman #%d)",
                     working_info_.spoolman_id);
            lv_label_set_text(spoolman_label, spoolman_text);
            lv_obj_remove_flag(spoolman_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(spoolman_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Build material options from filament database (if not already built).
    // When the active backend advertises a firmware whitelist (e.g., AD5X IFS
    // accepts only PLA / PLA-CF / SILK / TPU / ABS / PETG / PETG-CF), restrict
    // the dropdown to that set. Whitelist entries not present in the shared
    // filament DB (e.g., "SILK") are still appended so users aren't silently
    // locked out of a firmware-supported option.
    if (material_list_.empty()) {
        auto* backend = AmsState::instance().get_backend();
        auto supported = backend ? backend->get_supported_materials() : std::nullopt;
        const bool filtered = supported.has_value() && !supported->empty();

        auto to_lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return s;
        };

        auto is_supported = [&](const char* name) {
            if (!filtered) {
                return true;
            }
            std::string n_lc = to_lower(name);
            for (const auto& s : *supported) {
                if (to_lower(s) == n_lc) {
                    return true;
                }
            }
            return false;
        };

        auto all_materials = filament::get_all_material_names();
        material_list_.reserve(filtered ? supported->size() : all_materials.size());
        for (const char* mat : all_materials) {
            if (!is_supported(mat)) {
                continue;
            }
            if (!material_options_.empty()) {
                material_options_ += '\n';
            }
            material_options_ += mat;
            material_list_.push_back(mat);
        }

        // Ensure every whitelist entry appears even if the shared DB doesn't
        // have a case-matching name for it (e.g., AD5X's "SILK" vs DB's "Silk PLA").
        if (filtered) {
            for (const auto& s : *supported) {
                bool found = false;
                for (const auto& existing : material_list_) {
                    if (existing == s) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (!material_options_.empty()) {
                        material_options_ += '\n';
                    }
                    material_options_ += s;
                    material_list_.push_back(s);
                }
            }
        }

        spdlog::debug("[AmsEditModal] Built material list with {} materials (filtered={})",
                      material_list_.size(), filtered);
    }

    // Set up vendor dropdown (use cached vendors from Spoolman, or fallback)
    lv_obj_t* vendor_dropdown = find_widget("vendor_dropdown");
    if (vendor_dropdown) {
        if (!vendor_options_.empty()) {
            // Use vendors from Spoolman
            lv_dropdown_set_options(vendor_dropdown, vendor_options_.c_str());
        } else {
            // Fallback: static vendor list while Spoolman query is pending
            static const char* fallback_vendors =
                "Generic\nPolymaker\nBambu\neSUN\nOverture\nPrusa\nHatchbox";
            lv_dropdown_set_options(vendor_dropdown, fallback_vendors);

            // Build fallback vendor_list_ for index lookup (id=0 for static entries)
            if (vendor_list_.empty()) {
                for (const auto& name :
                     {"Generic", "Polymaker", "Bambu", "eSUN", "Overture", "Prusa", "Hatchbox"}) {
                    VendorInfo vi;
                    vi.name = name;
                    vendor_list_.push_back(std::move(vi));
                }
            }
        }

        // Set initial selection based on working_info_.brand, falling back to
        // "Generic" if the brand is empty or unknown. Normalize both working
        // and original to "Generic" when empty so the dropdown display matches
        // state and unchanged saves don't flip-flop the field.
        int vendor_idx = -1;
        int generic_idx = 0;
        for (size_t i = 0; i < vendor_list_.size(); i++) {
            if (vendor_list_[i].name == "Generic") {
                generic_idx = static_cast<int>(i);
            }
            if (!working_info_.brand.empty() && working_info_.brand == vendor_list_[i].name) {
                vendor_idx = static_cast<int>(i);
                break;
            }
        }
        if (vendor_idx < 0) {
            vendor_idx = generic_idx;
            if (working_info_.brand.empty())
                working_info_.brand = "Generic";
            if (original_info_.brand.empty())
                original_info_.brand = "Generic";
        }
        lv_dropdown_set_selected(vendor_dropdown, vendor_idx);
    }

    // Set up material dropdown from filament database
    lv_obj_t* material_dropdown = find_widget("material_dropdown");
    if (material_dropdown) {
        lv_dropdown_set_options(material_dropdown, material_options_.c_str());

        // Set initial selection based on working_info_.material
        int material_idx = 0; // Default to first (PLA)
        for (size_t i = 0; i < material_list_.size(); i++) {
            if (working_info_.material == material_list_[i]) {
                material_idx = static_cast<int>(i);
                break;
            }
        }
        lv_dropdown_set_selected(material_dropdown, material_idx);

        // Sync working_info_ when dropdown defaults to first entry
        if (working_info_.material.empty() && !material_list_.empty()) {
            working_info_.material = material_list_[material_idx];
        }
    }

    // Update color swatch
    lv_obj_t* color_swatch = find_widget("color_swatch");
    if (color_swatch) {
        lv_obj_set_style_bg_color(color_swatch, lv_color_hex(working_info_.color_rgb), 0);
    }

    // Update color name label via subject
    if (!working_info_.color_name.empty()) {
        snprintf(color_name_buf_, sizeof(color_name_buf_), "%s", working_info_.color_name.c_str());
    } else {
        color_name_buf_[0] = '\0';
    }
    lv_subject_copy_string(&color_name_subject_, color_name_buf_);

    // Update filament/spool product-line label. Hidden when empty so it
    // doesn't leave a blank row on backends/slots that don't populate it.
    lv_obj_t* spool_name_label = find_widget("spool_name_label");
    if (!working_info_.spool_name.empty()) {
        snprintf(spool_name_buf_, sizeof(spool_name_buf_), "%s", working_info_.spool_name.c_str());
        if (spool_name_label)
            lv_obj_remove_flag(spool_name_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        spool_name_buf_[0] = '\0';
        if (spool_name_label)
            lv_obj_add_flag(spool_name_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_subject_copy_string(&spool_name_subject_, spool_name_buf_);

    // Update remaining slider and label
    // Use synthetic 1000g total if no weight data (manual spool without Spoolman)
    if (working_info_.total_weight_g <= 0) {
        working_info_.total_weight_g = 1000.0f;
        working_info_.remaining_weight_g =
            (working_info_.remaining_weight_g > 0) ? working_info_.remaining_weight_g : 1000.0f;
    }
    int remaining_pct =
        static_cast<int>(100.0f * working_info_.remaining_weight_g / working_info_.total_weight_g);
    remaining_pct = std::max(0, std::min(100, remaining_pct));

    lv_obj_t* remaining_slider = find_widget("remaining_slider");
    if (remaining_slider) {
        lv_slider_set_value(remaining_slider, remaining_pct, LV_ANIM_OFF);
    }

    // Show/hide weight input based on whether we have real weight data
    lv_obj_t* weight_input = find_widget("remaining_weight_input");
    if (weight_input) {
        if (original_info_.total_weight_g > 0) {
            lv_obj_remove_flag(weight_input, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(weight_input, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update remaining label: "/ 1000g (75%)" or "75%" if no weight data
    format_remaining_label(remaining_pct);

    // Update progress bar fill width (shown in view mode)
    // Use percentage width to avoid layout timing issues
    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (progress_fill) {
        lv_obj_set_width(progress_fill, lv_pct(remaining_pct));
    }

    // Update temperature display based on material
    update_temp_display();

    // Populate tool dropdown with available tools
    // Show tool remap dropdown only for backends that support it
    lv_obj_t* tool_remap_row = find_widget("tool_remap_row");
    lv_obj_t* tool_dropdown = find_widget("tool_dropdown");
    auto* backend = AmsState::instance().get_backend();
    bool can_remap = backend && backend->get_system_info().supports_tool_mapping;

    if (tool_remap_row) {
        if (can_remap) {
            lv_obj_remove_flag(tool_remap_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tool_remap_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (tool_dropdown && can_remap) {
        int tool_count = static_cast<int>(backend->get_system_info().tool_to_slot_map.size());
        std::string tool_options;
        for (int i = 0; i < tool_count; i++) {
            if (!tool_options.empty()) {
                tool_options += '\n';
            }
            tool_options += "T" + std::to_string(i);
        }
        lv_dropdown_set_options(tool_dropdown, tool_options.c_str());

        // Set initial selection: T0 → index 0, T1 → index 1, etc.
        // Default to T0 if mapped_tool is -1 (shouldn't happen for remappable backends)
        int tool_idx = std::max(0, working_info_.mapped_tool);
        tool_idx = std::min(tool_idx, tool_count - 1);
        lv_dropdown_set_selected(tool_dropdown, tool_idx);
    }
}

void AmsEditModal::update_temp_display() {
    if (!dialog_) {
        return;
    }

    // Get temperature range from slot info (populated from Spoolman or material defaults)
    int nozzle_min = working_info_.nozzle_temp_min;
    int nozzle_max = working_info_.nozzle_temp_max;
    int bed_temp = working_info_.bed_temp;

    // Fall back to material-based defaults from filament database if not set
    if (nozzle_min == 0 && nozzle_max == 0 && !working_info_.material.empty()) {
        auto mat_info = filament::find_material(working_info_.material);
        if (mat_info) {
            nozzle_min = mat_info->nozzle_min;
            nozzle_max = mat_info->nozzle_max;
            bed_temp = mat_info->bed_temp;
            spdlog::debug("[AmsEditModal] Using filament database temps for {}: {}-{}°C nozzle, "
                          "{}°C bed",
                          working_info_.material, nozzle_min, nozzle_max, bed_temp);
        } else {
            // Fallback to PLA defaults for unknown materials
            auto pla_info = filament::find_material("PLA");
            if (pla_info) {
                nozzle_min = pla_info->nozzle_min;
                nozzle_max = pla_info->nozzle_max;
                bed_temp = pla_info->bed_temp;
            } else {
                // Ultimate fallback (should never happen - PLA is in database)
                nozzle_min = 200;
                nozzle_max = 230;
                bed_temp = 60;
            }
            spdlog::debug("[AmsEditModal] Material '{}' not in database, using PLA defaults",
                          working_info_.material);
        }
    }

    // Update nozzle temp label via subject
    snprintf(temp_nozzle_buf_, sizeof(temp_nozzle_buf_), "%d-%d°C", nozzle_min, nozzle_max);
    lv_subject_copy_string(&temp_nozzle_subject_, temp_nozzle_buf_);

    // Update bed temp label via subject
    snprintf(temp_bed_buf_, sizeof(temp_bed_buf_), "%d°C", bed_temp);
    lv_subject_copy_string(&temp_bed_subject_, temp_bed_buf_);
}

void AmsEditModal::format_remaining_label(int pct) {
    bool has_weight = original_info_.total_weight_g > 0;

    if (has_weight) {
        // Weight input field shows the remaining grams — label shows "/ Xg (Y%)"
        int total_g = static_cast<int>(working_info_.total_weight_g);
        snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "/ %dg (%d%%)", total_g, pct);
    } else {
        helix::format::format_percent(pct, remaining_pct_buf_, sizeof(remaining_pct_buf_));
    }
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    // Sync the weight input field if it exists and we have weight data
    if (has_weight) {
        lv_obj_t* weight_input = find_widget("remaining_weight_input");
        if (weight_input) {
            if (working_info_.remaining_weight_g < 0.0f) {
                // Sentinel: remaining weight unknown. Show blank rather than "-1"
                // (which is an internal sentinel, not a user-facing value).
                lv_textarea_set_text(weight_input, "");
            } else {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d",
                         static_cast<int>(working_info_.remaining_weight_g));
                lv_textarea_set_text(weight_input, buf);
            }
        }
    }
}

bool AmsEditModal::is_dirty() const {
    // Compare relevant fields that can be edited
    return working_info_.color_rgb != original_info_.color_rgb ||
           working_info_.material != original_info_.material ||
           working_info_.brand != original_info_.brand ||
           working_info_.spoolman_id != original_info_.spoolman_id ||
           working_info_.mapped_tool != original_info_.mapped_tool ||
           std::abs(working_info_.remaining_weight_g - original_info_.remaining_weight_g) > 0.1f;
}

void AmsEditModal::update_sync_button_state() {
    if (!dialog_) {
        return;
    }

    bool dirty = is_dirty();

    // Update save button text based on dirty state
    const char* btn_text = dirty ? lv_tr("Save") : lv_tr("Close");
    snprintf(save_btn_text_buf_, sizeof(save_btn_text_buf_), "%s", btn_text);
    lv_subject_copy_string(&save_btn_text_subject_, save_btn_text_buf_);
}

void AmsEditModal::show_color_picker() {
    if (!parent_) {
        spdlog::warn("[AmsEditModal] No parent for color picker");
        return;
    }

    // Create picker on first use (lazy initialization)
    if (!color_picker_) {
        color_picker_ = std::make_unique<ColorPicker>();
    }

    // Set callback to update edit modal when color is selected
    color_picker_->set_color_callback([this](uint32_t color_rgb, const std::string& color_name) {
        // Update the working slot info with selected color
        working_info_.color_rgb = color_rgb;
        working_info_.color_name = color_name;
        filament_user_edited_ = true; // genuine user edit gates new-spool create (#1071)

        // Update the edit modal's color swatch to show new selection
        if (dialog_) {
            lv_obj_t* swatch = find_widget("color_swatch");
            if (swatch) {
                lv_obj_set_style_bg_color(swatch, lv_color_hex(color_rgb), 0);
            }

            // Update color name label via subject
            snprintf(color_name_buf_, sizeof(color_name_buf_), "%s", color_name.c_str());
            lv_subject_copy_string(&color_name_subject_, color_name_buf_);

            update_sync_button_state();
        }
    });

    // Show with current edit color
    color_picker_->show_with_color(parent_, working_info_.color_rgb);
}

// ============================================================================
// Save Orchestration
// ============================================================================

void AmsEditModal::fire_completion(bool saved) {
    spdlog::info("[AmsEditModal] fire_completion saved={} slot={} spoolman_id={} material={}",
                 saved, slot_index_, working_info_.spoolman_id, working_info_.material);
    if (completion_callback_) {
        EditResult result;
        result.saved = saved;
        result.slot_index = slot_index_;
        result.slot_info = working_info_;
        spdlog::info("[AmsEditModal] Calling completion callback...");
        completion_callback_(result);
        spdlog::info("[AmsEditModal] Completion callback returned");
    }
    spdlog::info("[AmsEditModal] Calling hide()...");
    hide();
    spdlog::info("[AmsEditModal] hide() returned");
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsEditModal::handle_close() {
    spdlog::debug("[AmsEditModal] Close requested");
    fire_completion(false);
}

void AmsEditModal::handle_vendor_changed(int index) {
    if (index >= 0 && index < static_cast<int>(vendor_list_.size())) {
        working_info_.brand = vendor_list_[index].name;
        working_info_.spoolman_vendor_id = vendor_list_[index].id;
        filament_user_edited_ = true; // genuine user edit gates new-spool create (#1071)
        spdlog::debug("[AmsEditModal] Vendor changed to: {} (vendor_id={})", working_info_.brand,
                      working_info_.spoolman_vendor_id);
        update_sync_button_state();
    }
}

void AmsEditModal::handle_material_changed(int index) {
    if (index >= 0 && index < static_cast<int>(material_list_.size())) {
        working_info_.material = material_list_[index];
        filament_user_edited_ = true; // genuine user edit gates new-spool create (#1071)
        spdlog::debug("[AmsEditModal] Material changed to: {}", working_info_.material);

        // Clear existing temp values so update_temp_display uses material-based defaults
        working_info_.nozzle_temp_min = 0;
        working_info_.nozzle_temp_max = 0;
        working_info_.bed_temp = 0;

        // Update temperature display based on new material
        update_temp_display();
        update_sync_button_state();
    }
}

void AmsEditModal::handle_color_clicked() {
    spdlog::info("[AmsEditModal] Opening color picker");
    show_color_picker();
}

void AmsEditModal::handle_remaining_changed(int percent) {
    if (!dialog_) {
        return;
    }

    // Update the remaining label via format_remaining_label
    format_remaining_label(percent);

    // Update slot info remaining weight based on percentage
    // Use synthetic 1000g total if no weight data (manual spool without Spoolman)
    if (working_info_.total_weight_g <= 0) {
        working_info_.total_weight_g = 1000.0f;
    }
    working_info_.remaining_weight_g =
        working_info_.total_weight_g * static_cast<float>(percent) / 100.0f;

    update_sync_button_state();
    spdlog::trace("[AmsEditModal] Remaining changed to {}%", percent);
}

void AmsEditModal::handle_weight_input_changed() {
    if (!dialog_) {
        return;
    }

    lv_obj_t* weight_input = find_widget("remaining_weight_input");
    if (!weight_input) {
        return;
    }

    const char* text = lv_textarea_get_text(weight_input);
    if (!text || text[0] == '\0') {
        // Blank field: treat as "unknown weight" sentinel so save preserves -1
        // rather than coercing blank to 0. Reset the label to a plain percentage
        // display and leave the slider where it is.
        working_info_.remaining_weight_g = -1.0f;
        int total = static_cast<int>(working_info_.total_weight_g);
        if (total > 0) {
            snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "/ %dg", total);
        } else {
            remaining_pct_buf_[0] = '\0';
        }
        lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);
        update_sync_button_state();
        return;
    }

    int grams = std::atoi(text);
    if (grams < 0) {
        grams = 0;
    }
    int total_g = static_cast<int>(working_info_.total_weight_g);
    if (total_g > 0 && grams > total_g) {
        grams = total_g;
    }

    working_info_.remaining_weight_g = static_cast<float>(grams);

    // Recalculate percentage and update slider + label
    int pct = (working_info_.total_weight_g > 0)
                  ? static_cast<int>(100.0f * grams / working_info_.total_weight_g)
                  : 0;
    pct = std::max(0, std::min(100, pct));

    lv_obj_t* slider = find_widget("remaining_slider");
    if (slider) {
        lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    }

    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (progress_fill) {
        lv_obj_set_width(progress_fill, lv_pct(pct));
    }

    // Update the info label ("/ 1000g (75%)") without re-setting the input text
    int total = static_cast<int>(working_info_.total_weight_g);
    snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "/ %dg (%d%%)", total, pct);
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    update_sync_button_state();
    spdlog::trace("[AmsEditModal] Weight input changed to {}g ({}%)", grams, pct);
}

void AmsEditModal::handle_remaining_edit() {
    if (!dialog_) {
        return;
    }

    // Store current remaining percentage before entering edit mode
    lv_obj_t* slider = find_widget("remaining_slider");
    if (slider) {
        remaining_pre_edit_pct_ = lv_slider_get_value(slider);
    }

    // Enter edit mode - subject binding will show slider/accept/cancel, hide progress/edit button
    lv_subject_set_int(&remaining_mode_subject_, 1);
    spdlog::debug("[AmsEditModal] Entered remaining edit mode (was {}%)", remaining_pre_edit_pct_);
}

void AmsEditModal::handle_remaining_accept() {
    if (!dialog_) {
        return;
    }

    // Get the current slider value
    lv_obj_t* slider = find_widget("remaining_slider");
    int new_pct = slider ? lv_slider_get_value(slider) : remaining_pre_edit_pct_;

    // Update the progress bar fill to match
    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (progress_fill) {
        lv_obj_set_width(progress_fill, lv_pct(new_pct));
    }

    // Exit edit mode - subject binding will show progress/edit button, hide slider/accept/cancel
    lv_subject_set_int(&remaining_mode_subject_, 0);
    spdlog::debug("[AmsEditModal] Accepted remaining edit: {}%", new_pct);
}

void AmsEditModal::handle_remaining_cancel() {
    if (!dialog_) {
        return;
    }

    // Revert slider to pre-edit value
    lv_obj_t* slider = find_widget("remaining_slider");
    if (slider) {
        lv_slider_set_value(slider, remaining_pre_edit_pct_, LV_ANIM_OFF);
    }

    // Revert the remaining weight in working_info_ before updating label
    if (working_info_.total_weight_g > 0) {
        working_info_.remaining_weight_g =
            working_info_.total_weight_g * static_cast<float>(remaining_pre_edit_pct_) / 100.0f;
    }

    // Revert the remaining label via subject
    format_remaining_label(remaining_pre_edit_pct_);

    // Exit edit mode
    lv_subject_set_int(&remaining_mode_subject_, 0);
    update_sync_button_state();
    spdlog::debug("[AmsEditModal] Cancelled remaining edit (reverted to {}%)",
                  remaining_pre_edit_pct_);
}

void AmsEditModal::handle_tool_changed(int index) {
    // No "None" — index 0 = T0, index 1 = T1, etc.
    working_info_.mapped_tool = index;
    working_info_.tool_mapping_override = (index != original_info_.mapped_tool);
    spdlog::debug("[AmsEditModal] Tool changed to: T{} (override={})", index,
                  working_info_.tool_mapping_override);
    update_sync_button_state();
}

void AmsEditModal::handle_reset() {
    spdlog::debug("[AmsEditModal] Cancelling - discarding changes");

    // Discard changes and close
    working_info_ = original_info_;
    fire_completion(false);
}

bool AmsEditModal::should_create_new_spool(const SlotInfo& working_info,
                                           bool filament_user_edited) {
    // Unlinked + a genuine user edit + complete metadata. The user-edit term is
    // the #1071 Symptom C fix: an unedited open auto-defaults brand="Generic",
    // which alone satisfies is_filament_complete() and would otherwise spawn a
    // phantom spool on save.
    return working_info.spoolman_id == 0 && filament_user_edited &&
           helix::SpoolmanSlotSaver::is_filament_complete(working_info);
}

void AmsEditModal::handle_save() {
    spdlog::info("[AmsEditModal] Saving edits for slot {}", slot_index_);

    // Resolve API: prefer stored api_, fall back to global
    if (!api_) {
        api_ = get_moonraker_api();
    }

    // Sync active spool with Moonraker on every save — covers assignment changes
    // AND re-saves of an already-linked slot whose state Moonraker has lost
    // (e.g. after a Moonraker restart, a Spoolman outage, or an earlier create
    // path that didn't propagate the new ID). The underlying POST is idempotent
    // when the ID is unchanged. Fire-and-forget: local save proceeds regardless
    // of server response. Skipped for the new-spool-on-save path (working.id=0
    // until creation); that case re-syncs from the create callback below.
    if (api_ && working_info_.spoolman_id > 0) {
        sync_active_spool(api_, working_info_.spoolman_id);
    } else if (api_ && original_info_.spoolman_id > 0 && working_info_.spoolman_id == 0) {
        // Unassignment: propagate 0 so Moonraker clears its active spool.
        sync_active_spool(api_, 0);
    }

    // Delegate to SpoolmanSlotSaver whenever either:
    //   - There's a linked spool with pending edits (update filament/weight), OR
    //   - There's no linked spool but the user entered complete manual metadata
    //     (brand + material + non-default color) — create a new Spoolman spool.
    // SpoolmanSlotSaver::save() contains its own internal gates, so this is just
    // the outer guard that decides whether to invoke the async path at all.
    // Skip entirely if Spoolman isn't available on this printer — otherwise every
    // save would emit a "server.spoolman.proxy Method not found" error toast.
    if (api_ && get_printer_state().is_spoolman_available()) {
        const bool has_linked_spool = working_info_.spoolman_id > 0;
        auto changes = helix::SpoolmanSlotSaver::detect_changes(original_info_, working_info_);
        // Require an explicit user filament edit before creating a new Spoolman
        // spool. update_vendor_dropdown auto-defaults brand="Generic" on an
        // unedited open, which alone makes is_filament_complete() true; without
        // this gate an open+save with no edits spawns a phantom "Generic" spool
        // (#1071 Symptom C). Editing fields routes through handle_vendor_changed /
        // handle_material_changed / the color picker, which set the flag; picking
        // an existing spool (handle_spool_selected) does NOT — it takes the
        // has_linked_spool branch instead.
        const bool can_create_new = should_create_new_spool(working_info_, filament_user_edited_);

        // #1071 Symptom B: updating a LINKED spool whose filament identity changed
        // (different material, or color past the match tolerance) probably
        // clobbers a DIFFERENT physical spool's Spoolman definition. Confirm
        // first; the dismiss-safe default skips the Spoolman write (the slot still
        // saves locally and keeps its link, which the user can re-point via
        // "Choose Spool"). The clobber risk is backend-agnostic.
        if (has_linked_spool && changes.any() &&
            is_material_identity_change(original_info_, working_info_)) {
            prompt_identity_change_then_save();
            return;
        }

        if ((has_linked_spool && changes.any()) || can_create_new) {
            do_spoolman_save();
            return; // Async path - fire_completion called from callback
        }
    }

    // No Spoolman changes (or no Spoolman) - save locally immediately
    fire_completion(true);
}

void AmsEditModal::do_spoolman_save() {
    auto token = lifetime_.token();
    auto saver = std::make_shared<helix::SpoolmanSlotSaver>(api_);
    saver->save(
        original_info_, working_info_, [this, token, saver](const helix::SaveResult& result) {
            if (token.expired()) {
                return;
            }
            // Spoolman callback arrives on a background thread — defer
            // to the UI thread before touching LVGL subjects/widgets.
            token.defer([this, result]() {
                if (!result.success) {
                    // Local save still proceeds; only the Spoolman mirror failed.
                    spdlog::error("[AmsEditModal] Spoolman save failed, saving locally");
                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                  lv_tr("Couldn't update Spoolman — saved locally"),
                                                  3000);
                } else if (result.created_new_spool || result.repointed_filament) {
                    // Persist new Spoolman IDs into working_info_ so the
                    // completion callback's backend->set_slot_info() writes
                    // the link back to the slot. Without this, a subsequent
                    // edit would not know the spool exists and would create
                    // a duplicate.
                    if (result.new_spool_id != 0) {
                        working_info_.spoolman_id = result.new_spool_id;
                    }
                    if (result.new_filament_id != 0) {
                        working_info_.spoolman_filament_id = result.new_filament_id;
                    }
                    if (result.new_vendor_id != 0) {
                        working_info_.spoolman_vendor_id = result.new_vendor_id;
                    }
                    // The early sync_active_spool() above was skipped because
                    // spoolman_id was 0 on both sides (creation hadn't happened
                    // yet). Notify Moonraker now so Mainsail/Fluidd show the
                    // new spool as active and filament tracking starts.
                    if (result.created_new_spool && result.new_spool_id != 0 && api_) {
                        sync_active_spool(api_, result.new_spool_id);
                    }
                    if (result.created_new_spool) {
                        ToastManager::instance().show(ToastSeverity::INFO,
                                                      lv_tr("Added to Spoolman"), 2500);
                    }
                    // Repoint is silent — IDs change but no toast.
                }
                fire_completion(true);
            });
        });
}

bool AmsEditModal::is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
    if (!helix::FilamentMapper::materials_match(original.material, edited.material)) {
        return true;
    }
    return !helix::FilamentMapper::colors_match(original.color_rgb, edited.color_rgb);
}

void AmsEditModal::prompt_identity_change_then_save() {
    // Dismiss-safe binary confirmation. Confirm -> update the linked spool;
    // cancel/dismiss -> keep the linked Spoolman spool untouched and save the
    // slot locally (the stale link can be re-pointed via "Choose Spool").
    lv_obj_t* dlg = modal_show_confirmation(
        lv_tr("Different filament?"),
        lv_tr("This looks like a different spool than the one linked. Update the linked Spoolman "
              "spool anyway? Cancel keeps it unchanged."),
        ModalSeverity::Warning, lv_tr("Update anyway"), on_identity_confirm_cb,
        on_identity_cancel_cb, nullptr);
    if (!dlg) {
        // Couldn't show the dialog — fall back to the pre-gate behavior rather
        // than stranding the save (which would never fire_completion).
        spdlog::warn("[AmsEditModal] identity-change confirmation failed to show; updating anyway");
        do_spoolman_save();
    }
}

void AmsEditModal::on_identity_confirm_cb(lv_event_t* /*e*/) {
    if (s_active_instance_) {
        s_active_instance_->do_spoolman_save();
    }
}

void AmsEditModal::on_identity_cancel_cb(lv_event_t* /*e*/) {
    // Dismiss-safe: do NOT touch the linked Spoolman spool; save the slot locally.
    if (s_active_instance_) {
        s_active_instance_->fire_completion(true);
    }
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsEditModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"ams_edit_modal_close_cb", on_close_cb},
        {"ams_edit_vendor_changed_cb", on_vendor_changed_cb},
        {"ams_edit_material_changed_cb", on_material_changed_cb},
        {"ams_edit_color_clicked_cb", on_color_clicked_cb},
        {"ams_edit_remaining_changed_cb", on_remaining_changed_cb},
        {"ams_edit_remaining_edit_cb", on_remaining_edit_cb},
        {"ams_edit_remaining_accept_cb", on_remaining_accept_cb},
        {"ams_edit_remaining_cancel_cb", on_remaining_cancel_cb},
        {"ams_edit_reset_cb", on_reset_cb},
        {"ams_edit_save_cb", on_save_cb},
        {"ams_edit_manual_entry_cb", on_manual_entry_cb},
        {"ams_edit_change_spool_cb", on_change_spool_cb},
        {"ams_edit_spool_actions_clicked_cb", on_spool_actions_clicked_cb},
        {"ams_edit_spool_actions_changed_cb", on_spool_actions_changed_cb},
        {"ams_edit_scan_qr_cb", on_scan_qr_cb},
        {"ams_edit_picker_search_cb", on_picker_search_cb},
        {"ams_edit_picker_retry_cb", on_picker_retry_cb},
        // Register handler for spool_item clicks (shared component uses this callback name)
        {"spoolman_spool_item_clicked_cb", on_spool_item_cb},
        {"ams_edit_tool_changed_cb", on_tool_changed_cb},
        {"ams_edit_weight_changed_cb", on_weight_changed_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsEditModal] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via User Data)
// ============================================================================

AmsEditModal* AmsEditModal::s_active_instance_ = nullptr;

AmsEditModal* AmsEditModal::get_instance_from_event(lv_event_t* /*e*/) {
    // Use static active instance — only one edit modal can be open at a time.
    // The old parent-walk approach was unsafe: any ancestor with user_data
    // (e.g., panels, screens) would be miscast as AmsEditModal*.
    if (!s_active_instance_) {
        spdlog::warn("[AmsEditModal] Callback fired with no active instance");
    }
    return s_active_instance_;
}

void AmsEditModal::on_close_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_close();
    }
}

void AmsEditModal::on_vendor_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_vendor_changed(index);
    }
}

void AmsEditModal::on_material_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_material_changed(index);
    }
}

void AmsEditModal::on_color_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_color_clicked();
    }
}

void AmsEditModal::on_remaining_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int value = lv_slider_get_value(slider);
        self->handle_remaining_changed(value);
    }
}

void AmsEditModal::on_remaining_edit_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_edit();
    }
}

void AmsEditModal::on_remaining_accept_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_accept();
    }
}

void AmsEditModal::on_remaining_cancel_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_remaining_cancel();
    }
}

void AmsEditModal::on_reset_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_reset();
    }
}

void AmsEditModal::on_save_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_save();
    }
}

void AmsEditModal::on_manual_entry_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_manual_entry();
    }
}

void AmsEditModal::on_change_spool_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_change_spool();
    }
}

void AmsEditModal::on_spool_actions_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_scan_qr();
    }
}

void AmsEditModal::on_spool_actions_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (!self)
        return;

    auto* split_btn = self->find_widget("btn_spool_actions");
    if (!split_btn)
        return;
    uint32_t idx = ui_split_button_get_selected(split_btn);
    switch (idx) {
    case 0:
        self->handle_scan_qr();
        break;
    case 1:
        self->handle_spool_details();
        break;
    case 2:
        self->handle_unlink();
        break;
#if HELIX_HAS_LABEL_PRINTER
    case 3:
        self->handle_print_label();
        break;
#endif
    default:
        break;
    }
}

void AmsEditModal::on_scan_qr_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_scan_qr();
    }
}

void AmsEditModal::on_picker_search_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
        const char* text = lv_textarea_get_text(ta);
        self->handle_picker_search(text);
    }
}

void AmsEditModal::on_picker_retry_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        spdlog::info("[AmsEditModal] Picker retry requested by user");
        self->populate_picker();
    }
}

void AmsEditModal::on_tool_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_tool_changed(index);
    }
}

void AmsEditModal::on_weight_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_weight_input_changed();
    }
}

void AmsEditModal::on_spool_item_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (!self) {
        return;
    }

    // Use current_target (the button with the handler), not target (the clicked child)
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item) {
        return;
    }
    auto spool_id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
    if (spool_id <= 0) {
        spdlog::warn("[AmsEditModal] Spool item clicked with invalid spool_id={}", spool_id);
        return;
    }
    self->handle_spool_selected(spool_id);
}

} // namespace helix::ui

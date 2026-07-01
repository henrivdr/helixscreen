// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_context_menu.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_types.h"
#include "filament_database.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

// Static member initialization
bool AmsContextMenu::callbacks_registered_ = false;
AmsContextMenu* AmsContextMenu::s_active_instance_ = nullptr;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsContextMenu::AmsContextMenu() {
    // Initialize subjects for button enabled states
    lv_subject_init_int(&slot_is_loaded_subject_, 0);
    lv_xml_register_subject(nullptr, "ams_slot_is_loaded", &slot_is_loaded_subject_);

    lv_subject_init_int(&slot_can_load_subject_, 1);
    lv_xml_register_subject(nullptr, "ams_slot_can_load", &slot_can_load_subject_);

    subject_initialized_ = true;
    spdlog::debug("[AmsContextMenu] Constructed");
}

AmsContextMenu::~AmsContextMenu() {
    // Clear active instance before base destructor calls hide()
    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }

    // Clean up subjects
    if (subject_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&slot_is_loaded_subject_);
        lv_subject_deinit(&slot_can_load_subject_);
        subject_initialized_ = false;
    }
    spdlog::trace("[AmsContextMenu] Destroyed");
}

AmsContextMenu::AmsContextMenu(AmsContextMenu&& other) noexcept
    : ContextMenu(std::move(other)), action_callback_(std::move(other.action_callback_)),
      subject_initialized_(other.subject_initialized_), backend_(other.backend_),
      total_slots_(other.total_slots_), tool_dropdown_(other.tool_dropdown_),
      backup_dropdown_(other.backup_dropdown_), pending_is_loaded_(other.pending_is_loaded_) {
    // Transfer subject ownership
    if (other.subject_initialized_) {
        slot_is_loaded_subject_ = other.slot_is_loaded_subject_;
        slot_can_load_subject_ = other.slot_can_load_subject_;
    }
    // Update static instance
    if (s_active_instance_ == &other) {
        s_active_instance_ = this;
    }
    other.backend_ = nullptr;
    other.total_slots_ = 0;
    other.tool_dropdown_ = nullptr;
    other.backup_dropdown_ = nullptr;
    other.subject_initialized_ = false;
}

AmsContextMenu& AmsContextMenu::operator=(AmsContextMenu&& other) noexcept {
    if (this != &other) {
        // Clear our active instance before base hide()
        if (s_active_instance_ == this) {
            s_active_instance_ = nullptr;
        }

        // Let base class handle its state
        ContextMenu::operator=(std::move(other));

        action_callback_ = std::move(other.action_callback_);
        backend_ = other.backend_;
        total_slots_ = other.total_slots_;
        tool_dropdown_ = other.tool_dropdown_;
        backup_dropdown_ = other.backup_dropdown_;
        pending_is_loaded_ = other.pending_is_loaded_;

        // Transfer subject ownership
        if (other.subject_initialized_) {
            slot_is_loaded_subject_ = other.slot_is_loaded_subject_;
            slot_can_load_subject_ = other.slot_can_load_subject_;
        }
        subject_initialized_ = other.subject_initialized_;

        if (s_active_instance_ == &other) {
            s_active_instance_ = this;
        }

        other.backend_ = nullptr;
        other.total_slots_ = 0;
        other.tool_dropdown_ = nullptr;
        other.backup_dropdown_ = nullptr;
        other.subject_initialized_ = false;
    }
    return *this;
}

// ============================================================================
// Public API
// ============================================================================

void AmsContextMenu::set_action_callback(ActionCallback callback) {
    action_callback_ = std::move(callback);
}

bool AmsContextMenu::show_near_widget(lv_obj_t* parent, int slot_index, lv_obj_t* near_widget,
                                      bool is_loaded, AmsBackend* backend) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Store AMS-specific state BEFORE base class calls on_created
    backend_ = backend;
    pending_is_loaded_ = is_loaded;
    external_spool_mode_ = false;

    // Get total slots from backend
    if (backend_) {
        total_slots_ = backend_->get_system_info().total_slots;
    } else {
        total_slots_ = 0;
    }

    // Set as active instance for static callbacks
    s_active_instance_ = this;

    // Base class handles: XML creation, on_created callback, positioning
    bool result = ContextMenu::show_near_widget(parent, slot_index, near_widget);
    if (!result) {
        s_active_instance_ = nullptr;
    }

    spdlog::debug("[AmsContextMenu] Shown for slot {}", slot_index);
    return result;
}

bool AmsContextMenu::show_for_external_spool(lv_obj_t* parent, lv_obj_t* anchor_widget) {
    // Register callbacks once (idempotent)
    register_callbacks();

    // Configure for external spool mode (no backend operations)
    backend_ = nullptr;
    pending_is_loaded_ = false;
    total_slots_ = 0;
    external_spool_mode_ = true;

    // Set as active instance for static callbacks
    s_active_instance_ = this;

    // Base class handles: XML creation, on_created callback, positioning
    bool result = ContextMenu::show_near_widget(parent, -2, anchor_widget);
    if (!result) {
        s_active_instance_ = nullptr;
        external_spool_mode_ = false;
    }

    spdlog::debug("[AmsContextMenu] Shown for external spool");
    return result;
}

// ============================================================================
// ContextMenu override
// ============================================================================

void AmsContextMenu::on_created(lv_obj_t* menu_obj) {
    int slot_index = get_item_index();

    // External spool mode: hide backend-related buttons, show only EDIT/CLEAR
    if (external_spool_mode_) {
        // Hide Load, Unload, Reset buttons (not applicable to external spool)
        lv_obj_t* btn_load = lv_obj_find_by_name(menu_obj, "btn_load");
        if (btn_load)
            lv_obj_add_flag(btn_load, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t* btn_unload = lv_obj_find_by_name(menu_obj, "btn_unload");
        if (btn_unload)
            lv_obj_add_flag(btn_unload, LV_OBJ_FLAG_HIDDEN);
        // btn_reset_lane is already hidden by default in XML

        // Disable subject-driven states so hidden buttons stay hidden
        lv_subject_set_int(&slot_is_loaded_subject_, 0);
        lv_subject_set_int(&slot_can_load_subject_, 0);

        // Set header to "External Spool"
        lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
        if (slot_header) {
            lv_label_set_text(slot_header, lv_tr("External Spool"));
        }

        // Show Clear Spool button when external spool has an assignment.
        // btn_edit ("Spool Info") is always shown so users can reopen the edit modal.
        auto ext_info = AmsState::instance().get_external_spool_info();
        bool has_assignment =
            ext_info.has_value() && (ext_info->spoolman_id > 0 || !ext_info->material.empty());

        lv_obj_t* btn_clear = lv_obj_find_by_name(menu_obj, "btn_clear_spool");
        if (btn_clear && has_assignment) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }

        // Show "Select Spool" and "Scan QR Code" if Spoolman is available
        auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
        bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;

        lv_obj_t* btn_spoolman = lv_obj_find_by_name(menu_obj, "btn_spoolman");
        if (btn_spoolman && has_spoolman) {
            lv_obj_clear_flag(btn_spoolman, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t* btn_scan_qr = lv_obj_find_by_name(menu_obj, "btn_scan_qr");
        if (btn_scan_qr && has_spoolman) {
            lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
        }

        // No dropdowns for external spool
        return;
    }

    // Check if system is busy (operation in progress)
    bool system_busy = false;
    if (backend_) {
        AmsSystemInfo info = backend_->get_system_info();
        system_busy = (info.action != AmsAction::IDLE && info.action != AmsAction::ERROR);
        if (system_busy) {
            spdlog::debug("[AmsContextMenu] System busy ({}), disabling Load/Unload",
                          ams_action_to_string(info.action));
        }
    }

    // Get slot info for filament presence check
    bool slot_has_filament = false;
    // LIVE load state (Task 3): firmware seated+loaded OR filament at this slot's
    // toolhead sensor — drives Load/Unload availability instead of static RFID
    // presence so the menu matches real load state the moment a sensor flips.
    bool slot_is_loaded_live = false;
    if (backend_) {
        SlotInfo slot_info = backend_->get_slot_info(slot_index);
        slot_has_filament = slot_info.is_present();
        slot_is_loaded_live = backend_->slot_is_actively_loaded(slot_index) ||
                              backend_->slot_has_filament_at_toolhead(slot_index);
    }

    // Treat the slot as loaded if EITHER the caller's snapshot (can_unload_from_
    // toolhead, computed at open) OR the live per-slot accessors say so. The OR
    // keeps Unload available for the firmware's active slot even after a runout
    // clears the head sensor (#995), while live signals enable real-time accuracy.
    const bool is_loaded = pending_is_loaded_ || slot_is_loaded_live;

    // Whether this slot's unload action is a heated toolhead unload (true) or a
    // cold per-lane eject (false). Defaults to is_loaded for most backends; AD5X
    // IFS refines it with seated-channel authority so a NON-seated lane reads
    // "Eject" even when the firmware dropped its active-slot pointer and
    // is_loaded was broadened by the recovery clause (raza616, 5HR3HHS6). This
    // drives both the button label and the dispatched action (handle_unload).
    const bool toolhead_unload =
        backend_ ? backend_->slot_unloads_to_toolhead(slot_index, is_loaded) : is_loaded;

    // Determine eject mode: not a toolhead unload, but filament is in the lane,
    // and backend supports per-lane eject (AFC, Happy Hare, AD5X IFS)
    bool supports_eject = backend_ && backend_->supports_lane_eject();
    eject_mode_ = supports_eject && !toolhead_unload && slot_has_filament;

    // Determine force-eject/recover mode: idle lane reporting EMPTY, but backend
    // supports a cold presence-ignoring retract (AD5X IFS only) to recover a
    // snapped chunk stuck in the lane (#996). Mutually exclusive with eject_mode_:
    // eject requires filament present, force-eject requires the lane empty.
    bool slot_empty = !slot_has_filament;
    force_eject_mode_ =
        backend_ && backend_->supports_force_eject() && !toolhead_unload && slot_empty;

    // Update the unload/eject button label and state
    bool unload_eject_enabled = false;
    if (toolhead_unload) {
        // Loaded to toolhead → "Unload" enabled (disabled when NOT loaded)
        unload_eject_enabled = !system_busy;
    } else if (eject_mode_ || force_eject_mode_) {
        // Filament-in-lane eject, or empty-lane recover → enabled when idle
        unload_eject_enabled = !system_busy;
    }
    // else: no filament or eject not supported → disabled

    lv_subject_set_int(&slot_is_loaded_subject_, unload_eject_enabled ? 1 : 0);

    // Swap button label and icon to "Eject" when in eject mode
    if (eject_mode_) {
        lv_obj_t* btn_unload = lv_obj_find_by_name(menu_obj, "btn_unload");
        if (btn_unload) {
            ui_button_set_text(btn_unload, lv_tr("Eject"));
            ui_button_set_icon(btn_unload, "eject");
        }
    } else if (force_eject_mode_) {
        // Empty/runout lane: offer "Recover" (cold IFS_F11 retract) to clear a
        // snapped chunk the presence sensor can't see.
        lv_obj_t* btn_unload = lv_obj_find_by_name(menu_obj, "btn_unload");
        if (btn_unload) {
            ui_button_set_text(btn_unload, lv_tr("Recover"));
            ui_button_set_icon(btn_unload, "eject");
        }
    }

    // QIDI Box: a lane with ejectable filament but [force_move] enable_force_move
    // off means eject is unavailable (supports_lane_eject() is false). Surface a
    // one-line hint pointing at the config instead of silently omitting the
    // action — for QIDI, !supports_eject here can only mean force_move is off
    // (the box always supports eject otherwise). See #1041.
    if (backend_ && backend_->get_type() == AmsType::QIDI_BOX && !supports_eject &&
        !pending_is_loaded_ && slot_has_filament) {
        lv_obj_t* hint = lv_obj_find_by_name(menu_obj, "eject_force_move_hint");
        if (hint) {
            lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Determine if slot has filament for Load button state.
    // Gate on ACTUAL toolhead-loaded state (toolhead_unload), NOT the broadened
    // is_loaded recovery signal. is_loaded folds in can_unload_from_toolhead, which
    // reads true for the firmware's seated channel regardless of whether filament
    // actually reached the head. A cold-lane-eject lane (filament parked in the
    // lane but NOT at the toolhead) is a valid Load target, so it must stay
    // enabled. For non-AD5X backends slot_unloads_to_toolhead() returns the hint
    // unchanged, so !toolhead_unload == !is_loaded and behavior is unaffected.
    // Disable Load if: system busy, slot empty, OR filament is already at the head.
    bool can_load = !system_busy && !toolhead_unload && slot_has_filament;
    lv_subject_set_int(&slot_can_load_subject_, can_load ? 1 : 0);
    if (!can_load) {
        spdlog::debug("[AmsContextMenu] Load disabled for slot {}: busy={}, loaded={} "
                      "(live={}), has_filament={}",
                      slot_index, system_busy, is_loaded, slot_is_loaded_live, slot_has_filament);
    }

    // Show Reset Lane button if backend supports it
    if (backend_ && backend_->supports_lane_reset()) {
        lv_obj_t* btn_reset = lv_obj_find_by_name(menu_obj, "btn_reset_lane");
        if (btn_reset) {
            lv_obj_remove_flag(btn_reset, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_reset, LV_STATE_DISABLED);
            }
        }
    }

    // Show Select Gate button if backend supports it (e.g. Happy Hare)
    if (backend_ && backend_->supports_gate_select()) {
        lv_obj_t* btn_gate_select = lv_obj_find_by_name(menu_obj, "btn_gate_select");
        if (btn_gate_select) {
            lv_obj_remove_flag(btn_gate_select, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_gate_select, LV_STATE_DISABLED);
            }
        }
    }

    // Show Check Gate button if backend supports it (e.g. Happy Hare)
    if (backend_ && backend_->supports_gate_check()) {
        lv_obj_t* btn_gate_check = lv_obj_find_by_name(menu_obj, "btn_gate_check");
        if (btn_gate_check) {
            lv_obj_remove_flag(btn_gate_check, LV_OBJ_FLAG_HIDDEN);
            if (system_busy) {
                lv_obj_add_state(btn_gate_check, LV_STATE_DISABLED);
            }
        }
    }

    // Show Clear Spool button when an empty slot still has a sticky assignment.
    // btn_edit ("Spool Info") stays visible so users can reopen the edit modal
    // to correct metadata without first clearing.
    if (!slot_has_filament && backend_) {
        SlotInfo slot_info = backend_->get_slot_info(slot_index);
        bool has_assignment = (slot_info.spoolman_id > 0 || !slot_info.material.empty());
        lv_obj_t* btn_clear = lv_obj_find_by_name(menu_obj, "btn_clear_spool");
        if (btn_clear && has_assignment) {
            lv_obj_clear_flag(btn_clear, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Update the slot header text (1-based for user display)
    lv_obj_t* slot_header = lv_obj_find_by_name(menu_obj, "slot_header");
    if (slot_header) {
        char header_text[32];
        snprintf(header_text, sizeof(header_text), lv_tr("Slot %d"), slot_index + 1);
        lv_label_set_text(slot_header, header_text);
    }

    // Show "Scan QR Code" button if Spoolman is available
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    lv_obj_t* btn_scan_qr = lv_obj_find_by_name(menu_obj, "btn_scan_qr");
    if (btn_scan_qr && has_spoolman) {
        lv_obj_clear_flag(btn_scan_qr, LV_OBJ_FLAG_HIDDEN);
    }

    // Configure dropdowns based on backend capabilities
    configure_dropdowns();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsContextMenu::dispatch_ams_action(MenuAction action) {
    int slot = get_item_index();
    ActionCallback callback_copy = action_callback_;

    if (s_active_instance_ == this) {
        s_active_instance_ = nullptr;
    }
    hide();

    if (callback_copy) {
        callback_copy(action, slot);
    }
}

void AmsContextMenu::handle_backdrop_clicked() {
    spdlog::debug("[AmsContextMenu] Backdrop clicked");
    dispatch_ams_action(MenuAction::CANCELLED);
}

void AmsContextMenu::handle_load() {
    spdlog::info("[AmsContextMenu] Load requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::LOAD);
}

void AmsContextMenu::handle_unload() {
    if (eject_mode_ || force_eject_mode_) {
        spdlog::info("[AmsContextMenu] {} requested for slot {}",
                     force_eject_mode_ ? "Recover/force-eject" : "Eject", get_item_index());
        dispatch_ams_action(MenuAction::EJECT);
    } else {
        spdlog::info("[AmsContextMenu] Unload requested for slot {}", get_item_index());
        dispatch_ams_action(MenuAction::UNLOAD);
    }
}

void AmsContextMenu::handle_reset_lane() {
    spdlog::info("[AmsContextMenu] Reset lane requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::RESET_LANE);
}

void AmsContextMenu::handle_gate_select() {
    spdlog::info("[AmsContextMenu] Select gate requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SELECT_GATE);
}

void AmsContextMenu::handle_gate_check() {
    spdlog::info("[AmsContextMenu] Check gate requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CHECK_GATE);
}

void AmsContextMenu::handle_edit() {
    spdlog::info("[AmsContextMenu] Edit requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::EDIT);
}

void AmsContextMenu::handle_clear_spool() {
    spdlog::info("[AmsContextMenu] Clear spool requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::CLEAR_SPOOL);
}

void AmsContextMenu::handle_spoolman() {
    spdlog::info("[AmsContextMenu] Spoolman select requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SPOOLMAN);
}

void AmsContextMenu::handle_scan_qr() {
    spdlog::info("[AmsContextMenu] Scan QR requested for slot {}", get_item_index());
    dispatch_ams_action(MenuAction::SCAN_QR);
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsContextMenu::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    register_xml_callbacks({
        {"ams_context_backdrop_cb", on_backdrop_cb},
        {"ams_context_load_cb", on_load_cb},
        {"ams_context_unload_cb", on_unload_cb},
        {"ams_context_reset_lane_cb", on_reset_lane_cb},
        {"ams_context_gate_select_cb", on_gate_select_cb},
        {"ams_context_gate_check_cb", on_gate_check_cb},
        {"ams_context_edit_cb", on_edit_cb},
        {"ams_context_clear_spool_cb", on_clear_spool_cb},
        {"ams_context_spoolman_cb", on_spoolman_cb},
        {"ams_context_scan_qr_cb", on_scan_qr_cb},
        {"ams_context_tool_changed_cb", on_tool_changed_cb},
        {"ams_context_backup_changed_cb", on_backup_changed_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsContextMenu] Callbacks registered");
}

// ============================================================================
// Static Callbacks (Instance Lookup via Static Pointer)
// ============================================================================

AmsContextMenu* AmsContextMenu::get_active_instance() {
    if (!s_active_instance_) {
        spdlog::warn("[AmsContextMenu] No active instance for event");
    }
    return s_active_instance_;
}

void AmsContextMenu::on_backdrop_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_backdrop_clicked();
    }
}

void AmsContextMenu::on_load_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_load();
    }
}

void AmsContextMenu::on_unload_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_unload();
    }
}

void AmsContextMenu::on_reset_lane_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_reset_lane();
    }
}

void AmsContextMenu::on_gate_select_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gate_select();
    }
}

void AmsContextMenu::on_gate_check_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_gate_check();
    }
}

void AmsContextMenu::on_edit_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_edit();
    }
}

void AmsContextMenu::on_clear_spool_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_clear_spool();
    }
}

void AmsContextMenu::on_spoolman_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_spoolman();
    }
}

void AmsContextMenu::on_scan_qr_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_scan_qr();
    }
}

void AmsContextMenu::on_tool_changed_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_tool_changed();
    }
}

void AmsContextMenu::on_backup_changed_cb(lv_event_t* /*e*/) {
    auto* self = get_active_instance();
    if (self) {
        self->handle_backup_changed();
    }
}

// ============================================================================
// Dropdown Handlers
// ============================================================================

void AmsContextMenu::handle_tool_changed() {
    if (!tool_dropdown_ || !backend_) {
        return;
    }

    int selected = static_cast<int>(lv_dropdown_get_selected(tool_dropdown_));
    // Option 0 = "None", options 1+ = T0, T1, T2...
    int tool_number = selected - 1; // -1 = None

    spdlog::info("[AmsContextMenu] Tool mapping changed for slot {}: tool {}", get_item_index(),
                 tool_number >= 0 ? tool_number : -1);

    if (tool_number >= 0) {
        // Warn if another tool already maps to this slot
        auto mapping = backend_->get_tool_mapping();
        for (size_t i = 0; i < mapping.size(); ++i) {
            if (static_cast<int>(i) != tool_number && mapping[i] == get_item_index()) {
                spdlog::warn("[AmsContextMenu] Tool {} will share slot {} with tool {}",
                             tool_number, get_item_index(), i);
                std::string msg = fmt::format(lv_tr("T{} shares slot with T{}"), tool_number, i);
                ToastManager::instance().show(ToastSeverity::WARNING, msg.c_str());
                break;
            }
        }

        auto result = backend_->set_tool_mapping(tool_number, get_item_index());
        if (!result.success()) {
            spdlog::warn("[AmsContextMenu] Failed to set tool mapping: {}", result.user_msg);
            ToastManager::instance().show(ToastSeverity::ERROR, result.user_msg.c_str());
        }
    }
    // Note: "None" selection doesn't clear mapping - user needs to map another slot to that tool
}

void AmsContextMenu::handle_backup_changed() {
    if (!backup_dropdown_ || !backend_) {
        return;
    }

    int selected = static_cast<int>(lv_dropdown_get_selected(backup_dropdown_));

    // Convert dropdown index back to actual slot index
    // Dropdown: None=0, then all slots except current slot
    int backup_slot = -1; // Default to None
    if (selected > 0) {
        // Find the actual slot index by counting through slots (skipping current)
        int dropdown_idx = 0;
        for (int i = 0; i < total_slots_; ++i) {
            if (i != get_item_index()) {
                dropdown_idx++;
                if (dropdown_idx == selected) {
                    backup_slot = i;
                    break;
                }
            }
        }
    }

    // Validate material compatibility if a backup slot was selected
    if (backup_slot >= 0 && get_item_index() >= 0) {
        std::string current_material = backend_->get_slot_info(get_item_index()).material;
        std::string backup_material = backend_->get_slot_info(backup_slot).material;

        // Only check compatibility if both slots have materials set
        if (!current_material.empty() && !backup_material.empty() &&
            !filament::are_materials_compatible(current_material, backup_material)) {
            spdlog::warn("[AmsContextMenu] Incompatible backup: {} cannot use {} as backup",
                         current_material, backup_material);

            // Show toast error
            std::string msg =
                fmt::format(lv_tr("Incompatible materials: {} cannot use {} as backup"),
                            current_material, backup_material);
            ToastManager::instance().show(ToastSeverity::ERROR, msg.c_str());

            // Reset dropdown to "None" (index 0)
            lv_dropdown_set_selected(backup_dropdown_, 0);
            return;
        }
    }

    spdlog::info("[AmsContextMenu] Backup slot changed for slot {}: backup {}", get_item_index(),
                 backup_slot >= 0 ? backup_slot : -1);

    auto result = backend_->set_endless_spool_backup(get_item_index(), backup_slot);
    if (!result.success()) {
        spdlog::warn("[AmsContextMenu] Failed to set endless spool backup: {}", result.user_msg);
    } else {
        // Bump slots version to trigger endless spool arrow redraw
        AmsState::instance().bump_slots_version();
    }

    // Close the context menu after selection
    hide();
}

// ============================================================================
// Dropdown Configuration
// ============================================================================

void AmsContextMenu::configure_dropdowns() {
    if (!menu()) {
        return;
    }

    // Find dropdown widgets
    tool_dropdown_ = lv_obj_find_by_name(menu(), "tool_dropdown");
    backup_dropdown_ = lv_obj_find_by_name(menu(), "backup_dropdown");

    // Find row containers and divider
    lv_obj_t* tool_row = lv_obj_find_by_name(menu(), "tool_dropdown_row");
    lv_obj_t* backup_row = lv_obj_find_by_name(menu(), "backup_dropdown_row");
    lv_obj_t* divider = lv_obj_find_by_name(menu(), "dropdown_divider");

    bool show_any_dropdown = false;

    // Tool mapping dropdown - hidden until we have a good UX for remapping
    // (currently 1:1 lane-to-tool mapping is the only conflict-free option)
    // if (backend_) {
    //     auto tool_caps = backend_->get_tool_mapping_capabilities();
    //     if (tool_caps.supported) {
    //         populate_tool_dropdown();
    //         if (tool_row) {
    //             lv_obj_remove_flag(tool_row, LV_OBJ_FLAG_HIDDEN);
    //         }
    //         if (tool_dropdown_ && !tool_caps.editable) {
    //             lv_obj_add_state(tool_dropdown_, LV_STATE_DISABLED);
    //         }
    //         show_any_dropdown = true;
    //     }
    // }
    (void)tool_row;

    // Configure endless spool dropdown
    if (backend_) {
        auto es_caps = backend_->get_endless_spool_capabilities();
        if (es_caps.supported) {
            populate_backup_dropdown();
            if (backup_row) {
                lv_obj_remove_flag(backup_row, LV_OBJ_FLAG_HIDDEN);
            }
            // Disable dropdown if not editable
            if (backup_dropdown_ && !es_caps.editable) {
                lv_obj_add_state(backup_dropdown_, LV_STATE_DISABLED);
            }
            show_any_dropdown = true;
            spdlog::debug("[AmsContextMenu] Endless spool enabled (editable={})", es_caps.editable);
        }
    }

    // Show divider only if any dropdown is visible
    if (divider && show_any_dropdown) {
        lv_obj_remove_flag(divider, LV_OBJ_FLAG_HIDDEN);
    }
}

void AmsContextMenu::populate_tool_dropdown() {
    if (!tool_dropdown_) {
        return;
    }

    std::string options = build_tool_options();
    lv_dropdown_set_options(tool_dropdown_, options.c_str());

    int current_tool = get_current_tool_for_slot();
    // Map tool number to dropdown index: None=0, T0=1, T1=2, etc.
    int selected_index = (current_tool >= 0) ? (current_tool + 1) : 0;
    lv_dropdown_set_selected(tool_dropdown_, static_cast<uint32_t>(selected_index));

    spdlog::debug("[AmsContextMenu] Tool dropdown populated: slot {} maps to tool {}",
                  get_item_index(), current_tool);
}

void AmsContextMenu::populate_backup_dropdown() {
    if (!backup_dropdown_) {
        return;
    }

    std::string options = build_backup_options();
    lv_dropdown_set_options(backup_dropdown_, options.c_str());

    int current_backup = get_current_backup_for_slot();
    // Map backup slot to dropdown index, accounting for skipped current slot
    // Dropdown: None=0, then all slots except current slot
    int selected_index = 0; // Default to None
    if (current_backup >= 0) {
        // Count how many slots appear before the backup slot in the dropdown
        // (which skips the current slot)
        selected_index = 1; // Start after "None"
        for (int i = 0; i < current_backup; ++i) {
            if (i != get_item_index()) {
                selected_index++;
            }
        }
    }
    lv_dropdown_set_selected(backup_dropdown_, static_cast<uint32_t>(selected_index));

    spdlog::debug("[AmsContextMenu] Backup dropdown populated: slot {} backup is {}",
                  get_item_index(), current_backup);
}

std::string AmsContextMenu::build_tool_options() const {
    std::string options = lv_tr("None");
    // Add tool options T0, T1, T2... based on total slots
    for (int i = 0; i < total_slots_; ++i) {
        options += "\nT" + std::to_string(i);
    }
    return options;
}

std::string AmsContextMenu::build_backup_options() const {
    std::string options = lv_tr("None");

    // Get current slot's material for compatibility checking
    std::string current_material;
    if (backend_ && get_item_index() >= 0) {
        current_material = backend_->get_slot_info(get_item_index()).material;
    }

    // Add slot options Slot 1, Slot 2... based on total slots
    // Skip the current slot (can't be backup for itself)
    // Mark incompatible materials
    for (int i = 0; i < total_slots_; ++i) {
        if (i != get_item_index()) {
            std::string slot_option = "\n" + fmt::format(lv_tr("Slot {}"), i + 1);

            // Check material compatibility if we have a current material
            if (backend_ && !current_material.empty()) {
                std::string other_material = backend_->get_slot_info(i).material;
                if (!other_material.empty() &&
                    !filament::are_materials_compatible(current_material, other_material)) {
                    slot_option += std::string(" ") + lv_tr("(incompatible)");
                }
            }

            options += slot_option;
        }
    }
    return options;
}

int AmsContextMenu::get_current_tool_for_slot() const {
    if (!backend_) {
        return -1;
    }

    // Get tool mapping and find which tool maps to this slot
    auto mapping = backend_->get_tool_mapping();
    for (size_t tool = 0; tool < mapping.size(); ++tool) {
        if (mapping[tool] == get_item_index()) {
            return static_cast<int>(tool);
        }
    }
    return -1; // No tool maps to this slot
}

int AmsContextMenu::get_current_backup_for_slot() const {
    if (!backend_) {
        return -1;
    }

    auto configs = backend_->get_endless_spool_config();
    for (const auto& config : configs) {
        if (config.slot_index == get_item_index()) {
            return config.backup_slot;
        }
    }
    return -1; // No backup configured
}

} // namespace helix::ui

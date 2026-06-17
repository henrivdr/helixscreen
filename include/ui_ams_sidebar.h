// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_clog_meter.h"
#include "ui_observer_guard.h"

#include "ams_backend.h"
#include "ams_step_operation.h"
#include "ams_types.h"

#include <memory>
#include <vector>

// Forward declarations
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_event_t;
typedef struct _lv_event_t lv_event_t;

namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @brief Shared AMS sidebar component for operation status and controls
 *
 * Manages the right-column sidebar used by both AmsPanel and AmsOverviewPanel.
 * Contains: current loaded card, status display, step progress stepper,
 * action buttons (unload/reset/settings/bypass), and dryer card.
 *
 * Uses user_data callback routing pattern (same as AmsDryerCard).
 * Static callbacks traverse parent chain to find the AmsOperationSidebar instance.
 */
class AmsOperationSidebar {
  public:
    explicit AmsOperationSidebar(PrinterState& ps);
    ~AmsOperationSidebar();

    // Non-copyable, non-movable
    AmsOperationSidebar(const AmsOperationSidebar&) = delete;
    AmsOperationSidebar& operator=(const AmsOperationSidebar&) = delete;

    /**
     * @brief Find sidebar widget in panel, set user_data, setup dryer card
     * @param panel Root panel object containing the ams_sidebar component
     * @return true if sidebar found and initialized
     */
    bool setup(lv_obj_t* panel);

    /**
     * @brief Register action/current_slot/extruder_temp observers
     */
    void init_observers();

    /**
     * @brief Clear observers and widget refs
     *
     * Unconditionally resets ALL observers and nullifies widget pointers.
     * Widget pointers are cleared before observers to prevent cascading
     * observer callbacks from accessing freed LVGL objects.
     */
    void cleanup();

    /**
     * @brief Sync step progress and swatch from current state (call on panel activate)
     */
    void sync_from_state();

    /**
     * @brief Start an operation with known type and target slot
     *
     * Called BEFORE backend operation to set up step progress and pulse animation.
     * Sets action to HEATING and shows step progress immediately.
     */
    void start_operation(StepOperationType op_type, int target_slot);

    /**
     * @brief Handle load request with automatic preheat if needed
     */
    void handle_load_with_preheat(int slot_index);

    /**
     * @brief Handle unload of a specific slot from the context menu.
     *
     * Routes through start_operation(UNLOAD) so the vertical step widget is
     * built correctly (mirrors how handle_load_with_preheat routes LOAD).
     * Without this the context-menu UNLOAD path called the backend directly,
     * leaving auto-detect to mis-build the stepper as LOAD_SWAP.
     */
    void handle_unload(int slot_index);

    /**
     * @brief Update the loaded card swatch color and info
     */
    void update_current_loaded_display();

    /**
     * @brief Hide settings button if backend has no device sections
     */
    void update_settings_visibility();

    /**
     * @brief Show/hide Check gates button based on whether the active backend supports it.
     */
    void update_check_gates_visibility();

    /**
     * @brief Set btn_reset's label from the active backend (e.g. "Home" for Happy Hare).
     */
    void sync_reset_button_label();

    /**
     * @brief Register XML event callbacks (call once before XML parsing)
     */
    static void register_callbacks_static();

  private:
    // Dependencies
    PrinterState& printer_state_;

    // Widget references
    lv_obj_t* sidebar_root_ = nullptr;
    lv_obj_t* step_progress_ = nullptr;
    lv_obj_t* step_progress_container_ = nullptr;

    // Extracted UI modules
    std::unique_ptr<UiClogMeter> clog_meter_;

    // Bypass spool observer (updates sidebar if needed)
    ObserverGuard bypass_spool_observer_;

    // Observers
    ObserverGuard action_observer_;
    ObserverGuard current_slot_observer_;
    ObserverGuard active_backend_observer_;
    ObserverGuard extruder_temp_observer_;
    ObserverGuard extruder_target_observer_;
    ObserverGuard color_observer_;
    // Drives the step bar's current step on the Snapmaker U1 from the granular
    // firmware phase (Home/Select/Heat/Move) instead of the coarse AmsAction.
    // ams_operation_phase is a STATIC singleton subject (one per AmsState), so
    // no SubjectLifetime token is required — cleaned up via reset() in cleanup().
    ObserverGuard operation_phase_observer_;

    // Observes AmsState's toolchange_step subject (static singleton — member
    // ObserverGuard alone is correct, no SubjectLifetime needed). The
    // GcodeNarrationRouter drives this index from `//` narration lines; the
    // observer highlights the matching step row and scrolls it into view.
    ObserverGuard toolchange_step_observer_;
    // True when the active backend supplied a non-empty toolchange phase
    // template, so the step bar is driven by narration rather than the coarse
    // AmsAction enum. Suppresses the legacy AmsAction→index advancement.
    bool narration_driven_ = false;
    // Keeps the current backend phase template alive. (ui_step_progress_create
    // copies label strings into its own buffers, so this is not strictly
    // required for c_str() validity, but it documents the active template and
    // is harmless.)
    std::vector<AmsBackend::ToolchangePhase> current_phase_template_;

    // Bypass-after-unload state
    bool pending_bypass_enable_ = false;

    // Preheat state
    int pending_load_slot_ = -1;
    int pending_load_target_temp_ = 0;
    bool ui_initiated_heat_ = false;
    AmsAction prev_ams_action_ = AmsAction::IDLE;

    // Lifecycle flag — set in setup(), cleared in cleanup().
    // Guards widget operations against use-after-free on dangling lv_obj_t* pointers.
    bool active_ = false;

    // Step progress state
    StepOperationType current_operation_type_ = StepOperationType::LOAD_FRESH;
    int current_step_count_ = 4;
    int target_load_slot_ = -1;
    bool heat_label_showing_temp_ = false;
    // Index of the Heat step in the Snapmaker 4-phase bar (Home/Select/Heat/Move).
    // The phase subject reports this index when the firmware is heating.
    static constexpr int kSnapmakerHeatStep = 2;
    // Whether the current LOAD_SWAP/UNLOAD stepper includes a discrete tip
    // (cut / tip-form) step. False for backends with TipMethod::NONE (e.g. the
    // Snapmaker U1, which only heats + retracts). Drives the step-index map in
    // get_step_index_for_action so the trailing steps don't shift out of place.
    bool current_op_has_tip_step_ = true;

    // Step progress methods
    void setup_step_progress();
    void recreate_step_progress_for_operation(StepOperationType op_type);
    void update_step_progress(AmsAction action);
    int get_step_index_for_action(AmsAction action, StepOperationType op_type);

    // True when the active backend is the Snapmaker U1, whose granular firmware
    // phases drive a dedicated 4-step bar (Home/Select/Heat/Move).
    static bool active_backend_is_snapmaker();

    // Translate a base op_type (LOAD_FRESH/LOAD_SWAP/UNLOAD) to its Snapmaker
    // four-phase variant. Pass-through for non-load/unload types.
    static StepOperationType to_snapmaker_op_type(StepOperationType base);

    // Apply the granular firmware phase (0..3, -1=none) to the current Snapmaker
    // step bar. No-op unless the current operation is a SNAPMAKER_* stepper.
    void apply_snapmaker_phase(int phase);

    // Update the Snapmaker Heat step (index kSnapmakerHeatStep) label: a live
    // "Heat nozzle X / Y°C" readout while on the Heat phase, reverting to the
    // static "Heat nozzle" label otherwise. Driven by apply_snapmaker_phase and
    // the extruder temp/target observers (live updates while heating).
    void refresh_snapmaker_heat_label(int phase);

    // Re-evaluate step display when extruder temp/target changes (called by observers).
    // Physical heating state overrides AmsAction for step indicator: backends emit
    // LOADING optimistically at gcode dispatch (CFS, ACE, AD5x), and even firmware-driven
    // backends can fire the next phase before the printer leaves heating.
    void refresh_heat_step_display();
    bool is_extruder_below_target() const;

    // Preheat methods
    int get_load_temp_for_slot(int slot_index);
    void check_pending_load();
    void handle_load_complete();
    void show_preheat_feedback(int slot_index, int target_temp);

    // Action handlers
    void handle_unload();
    void handle_reset();
    void handle_check_gates();
    void handle_bypass_toggle();

    // Action display (sidebar-relevant parts only)
    void update_action_display(AmsAction action);

    // Static callback routing
    static AmsOperationSidebar* get_instance_from_event(lv_event_t* e);

    // Static XML callbacks
    static void on_bypass_toggled_cb(lv_event_t* e);
    static void on_unload_clicked_cb(lv_event_t* e);
    static void on_reset_clicked_cb(lv_event_t* e);
    static void on_check_gates_clicked_cb(lv_event_t* e);
    static void on_settings_clicked_cb(lv_event_t* e);
};

} // namespace helix::ui

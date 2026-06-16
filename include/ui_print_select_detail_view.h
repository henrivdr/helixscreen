// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_filament_mapping_card.h"
#include "ui_pre_print_options_renderer.h"
#include "ui_print_preparation_manager.h"

#include "preflight_validator.h"

#include "moonraker_types.h"
#include "overlay_base.h"
#include "print_file_data.h" // For FileHistoryStatus
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <optional>
#include <set>
#include <string>

// Forward declarations
class MoonrakerAPI;
namespace helix {
class PrinterState;
}

namespace helix::ui {

/**
 * @file ui_print_select_detail_view.h
 * @brief Detail view overlay manager for print selection panel
 *
 * Handles the file detail overlay that appears when a file is selected,
 * including:
 * - Creating and positioning the detail view widget
 * - Showing/hiding with nav system integration
 * - Delete confirmation modal management
 * - Filament type dropdown synchronization
 *
 * ## Usage:
 * @code
 * PrintSelectDetailView detail_view;
 * detail_view.create(parent_screen);
 * detail_view.set_prep_manager(prep_manager);
 * detail_view.set_on_delete_confirmed([this]() { delete_file(); });
 *
 * // When file selected:
 * detail_view.show(filename, current_path, filament_type);
 *
 * // When back button clicked:
 * detail_view.hide();
 * @endcode
 */

/**
 * @brief Callback when delete is confirmed
 */
using DeleteConfirmedCallback = std::function<void()>;

/**
 * @brief Detail view overlay manager
 *
 * Inherits from OverlayBase for lifecycle management (on_activate/on_deactivate).
 * The NavigationManager calls these hooks automatically when the overlay is
 * pushed/popped from the stack.
 */
class PrintSelectDetailView : public OverlayBase {
  public:
    PrintSelectDetailView() = default;
    ~PrintSelectDetailView() override;

    // Non-copyable, non-movable (owns LVGL widgets with external references)
    PrintSelectDetailView(const PrintSelectDetailView&) = delete;
    PrintSelectDetailView& operator=(const PrintSelectDetailView&) = delete;
    PrintSelectDetailView(PrintSelectDetailView&&) = delete;
    PrintSelectDetailView& operator=(PrintSelectDetailView&&) = delete;

    // === OverlayBase Interface ===

    /**
     * @brief Initialize subjects for pre-print option switches
     *
     * Creates and registers subjects that control switch default states.
     * Skip switches (bed_mesh, qgl, z_tilt, nozzle_clean) default to ON.
     * Add-on switches (timelapse) default to OFF.
     *
     * MUST be called BEFORE create() so bindings can find subjects.
     */
    void init_subjects() override;

    /**
     * @brief Create the detail view widget
     *
     * Creates the print_file_detail XML component and configures it.
     * Must be called before show().
     *
     * @param parent_screen Screen to create detail view on
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent_screen) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Print File Details"
     */
    const char* get_name() const override {
        return "Print File Details";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Resets pre-print subjects to defaults and starts async file scanning.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Closes any open modals, pauses gcode viewer.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     *
     * Invalidates lifetime tokens so async callbacks bail out.
     * Unregisters from NavigationManager and deinitializes subjects.
     */
    void cleanup() override;

    // === Setup ===

    /**
     * @brief Set dependencies for print preparation
     *
     * @param api MoonrakerAPI for file operations
     * @param printer_state PrinterState for capability detection
     */
    void set_dependencies(MoonrakerAPI* api, PrinterState* printer_state);

    /**
     * @brief Set callback for delete confirmation
     */
    void set_on_delete_confirmed(DeleteConfirmedCallback callback) {
        on_delete_confirmed_ = std::move(callback);
    }

    /**
     * @brief Set the visible subject for XML binding
     *
     * The subject should be initialized to 0 (hidden).
     */
    void set_visible_subject(lv_subject_t* subject) {
        visible_subject_ = subject;
    }

    // === Visibility ===

    /**
     * @brief Show the detail view overlay
     *
     * Pushes overlay via nav system and triggers G-code scanning.
     *
     * @param filename Selected filename (for G-code scanning)
     * @param current_path Current directory path
     * @param filament_type Filament type from metadata (for dropdown default)
     * @param filament_colors Optional tool colors for multi-color prints
     * @param file_size_bytes File size from Moonraker metadata (for safety checks)
     */
    void show(const std::string& filename, const std::string& current_path,
              const std::string& filament_type,
              const std::vector<std::string>& filament_colors = {},
              const std::vector<std::string>& filament_materials = {}, size_t file_size_bytes = 0);

    /**
     * @brief Hide the detail view overlay
     *
     * Uses nav system to properly hide with backdrop management.
     */
    void hide();

    /**
     * @brief Whether the currently-shown file references any tool whose
     *        AMS lane/slot is empty. Backend-agnostic — driven by the cached
     *        PreflightResult (has_block(): any tool mapped to an empty slot),
     *        computed for ALL backends in try_extract_gcode_colors().
     *        Used by the print-start path to confirm with the user before
     *        sending a print that will likely fail mid-flight.
     */
    [[nodiscard]] bool has_empty_tool_warning() {
        return lv_subject_get_int(&empty_tools_warning_) == 1;
    }

    // Note: is_visible() inherited from OverlayBase

    // === Delete Confirmation ===

    /**
     * @brief Show delete confirmation dialog
     *
     * @param filename Filename to display in confirmation message
     */
    void show_delete_confirmation(const std::string& filename);

    /**
     * @brief Hide delete confirmation dialog
     */
    void hide_delete_confirmation();

    // === Widget Access ===

    /**
     * @brief Get the detail view widget
     * @note Returns overlay_root_ from OverlayBase
     */
    [[nodiscard]] lv_obj_t* get_widget() const {
        return overlay_root_;
    }

    /**
     * @brief Get the print button (for enable/disable state)
     */
    [[nodiscard]] lv_obj_t* get_print_button() const {
        return print_button_;
    }

    /**
     * @brief Get the print preparation manager
     */
    [[nodiscard]] PrintPreparationManager* get_prep_manager() const {
        return prep_manager_.get();
    }

    /**
     * @brief Get current filament mappings from the mapping card
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_filament_mappings() const {
        return filament_mapping_card_.get_mappings();
    }

    /**
     * @brief Get per-tool gcode info from the mapping card
     */
    [[nodiscard]] std::vector<helix::GcodeToolInfo> get_filament_tool_info() const {
        return filament_mapping_card_.get_tool_info();
    }

    /**
     * @brief Get per-tool filament materials from gcode metadata
     *
     * Available even when AMS is not present (unlike get_filament_tool_info
     * which relies on the mapping card).
     */
    [[nodiscard]] const std::vector<std::string>& get_filament_materials() const {
        return current_filament_materials_;
    }

    /**
     * @brief Get available AMS slots from the mapping card
     */
    [[nodiscard]] const std::vector<helix::AvailableSlot>& get_available_slots() const {
        return filament_mapping_card_.get_available_slots();
    }

    /**
     * @brief Get the cached pre-flight validation result for the current file.
     *
     * Computed in try_extract_gcode_colors() after the gcode is parsed, for
     * ALL AMS backends (including those whose mapping card is hidden, e.g.
     * Snapmaker U1 / ACE). Drives the filament_mismatch_ and
     * empty_tools_warning_ subjects, and is read by the print-start gate and
     * the enriched pre-print modal. Empty (no checks) until a file is parsed.
     */
    [[nodiscard]] const helix::PreflightResult& preflight_result() const {
        return preflight_result_;
    }

    /**
     * @brief Get cached file metadata from the most recent async fetch
     *
     * Populated after the metadata fetch completes. Returns nullopt if the
     * user opened the detail view before the fetch finished.
     */
    [[nodiscard]] std::optional<FileMetadata> get_file_metadata() const {
        return cached_file_metadata_;
    }

    // === Checkbox Access (for prep manager setup) ===

    [[nodiscard]] lv_obj_t* get_bed_mesh_checkbox() const {
        return bed_mesh_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_qgl_checkbox() const {
        return qgl_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_z_tilt_checkbox() const {
        return z_tilt_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_nozzle_clean_checkbox() const {
        return nozzle_clean_checkbox_;
    }
    [[nodiscard]] lv_obj_t* get_timelapse_checkbox() const {
        return timelapse_checkbox_;
    }

    // === Subject Access ===

    [[nodiscard]] lv_subject_t* get_prep_time_estimate_subject() {
        return &prep_time_estimate_subject_;
    }

    // === Resize Handling ===

    /**
     * @brief Handle resize event - update responsive padding
     *
     * @param parent_screen Parent screen for height calculation
     */
    void handle_resize(lv_obj_t* parent_screen);

    /**
     * @brief Update the print history status display
     *
     * @param status The history status (NEVER_PRINTED, CURRENTLY_PRINTING, COMPLETED, FAILED)
     * @param success_count Number of successful prints (used when status is COMPLETED)
     */
    void update_history_status(FileHistoryStatus status, int success_count);

  protected:
    /**
     * @brief Called after widget tree is destroyed by destroy_overlay_ui()
     *
     * Nulls all child widget pointers so that create() works correctly
     * when re-invoked on next open. Also invalidates lifetime tokens
     * so in-flight async callbacks bail out (they may reference stale
     * widget pointers like gcode_viewer_).
     */
    void on_ui_destroyed() override;

  private:
    // === Dependencies ===
    MoonrakerAPI* api_ = nullptr;
    PrinterState* printer_state_ = nullptr;
    lv_subject_t* visible_subject_ = nullptr;

    // === Widget References ===
    // Note: overlay_root_ inherited from OverlayBase holds the main widget
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* confirmation_dialog_widget_ = nullptr;
    lv_obj_t* print_button_ = nullptr;
    lv_obj_t* gcode_viewer_ = nullptr;

    // Pre-print option checkboxes
    lv_obj_t* bed_mesh_checkbox_ = nullptr;
    lv_obj_t* qgl_checkbox_ = nullptr;
    lv_obj_t* z_tilt_checkbox_ = nullptr;
    lv_obj_t* nozzle_clean_checkbox_ = nullptr;
    lv_obj_t* purge_line_checkbox_ = nullptr;
    lv_obj_t* timelapse_checkbox_ = nullptr;

    // Color swatches container (parent card visibility driven by the
    // color_swatches_visible subject — bound in print_file_detail.xml).
    lv_obj_t* color_swatches_row_ = nullptr;

    // History status display
    lv_obj_t* history_status_row_ = nullptr;
    lv_obj_t* history_status_icon_ = nullptr;
    lv_obj_t* history_status_label_ = nullptr;

    // G-code viewer visibility mode (0=thumbnail, 1=3D, 2=2D)
    lv_subject_t detail_gcode_viewer_mode_{};
    // G-code loading indicator (0=hidden, 1=visible)
    lv_subject_t detail_gcode_loading_{};
    std::string temp_gcode_path_; // Cached downloaded gcode file path
    bool gcode_loaded_ = false;   // Whether gcode file has been loaded into viewer

    // Pre-print option toggle state lives in `option_rows_renderer_` (one
    // heap-allocated subject per option) — the legacy fixed six subjects
    // (preprint_bed_mesh_, preprint_qgl_, etc.) were retired in Phase 3.5.
    lv_subject_t filament_mismatch_{};          // 1 = material mismatch warning visible
    lv_subject_t filament_mapping_visible_{};   // 1 = filament mapping card visible (AMS+tools)
    lv_subject_t color_swatches_visible_{};     // 1 = legacy color swatches card visible
    lv_subject_t empty_tools_warning_{};        // 1 = at least one used tool's slot is empty
    // Cached backend-agnostic pre-flight validation result for the current file.
    // Computed in try_extract_gcode_colors() once the gcode is parsed; the single
    // source of truth driving filament_mismatch_ + empty_tools_warning_ (works
    // even when the mapping card is hidden, e.g. Snapmaker U1 / ACE). Read by the
    // print-start gate and the enriched pre-print modal. Empty until a file parses.
    helix::PreflightResult preflight_result_{};
    lv_subject_t prep_time_estimate_subject_{}; // formatted prep time string for bind_text
    char prep_time_estimate_buf_[64]{};         // buffer backing the string subject
    SubjectManager subjects_;                   // RAII manager for subject cleanup
    // Note: subjects_initialized_ inherited from OverlayBase

    // Print preparation manager (owns it)
    std::unique_ptr<PrintPreparationManager> prep_manager_;

    // Filament mapping card (replaces color swatches when AMS available)
    FilamentMappingCard filament_mapping_card_;

    // Dynamically-built option toggle rows for the active printer's
    // PrePrintOptionSet. Populated in on_activate() and rebuilt when the
    // printer type changes. Owns per-option state subjects.
    PrePrintOptionsRenderer option_rows_renderer_;
    lv_obj_t* pre_print_options_container_ = nullptr;
    std::string last_rendered_printer_type_;

    // === Cached show() parameters (used by on_activate) ===
    std::string current_filename_;
    std::string current_path_;
    std::string current_filament_type_;
    std::vector<std::string> current_filament_colors_;
    std::vector<std::string> current_filament_materials_;
    size_t current_file_size_bytes_ = 0;

    // Cached metadata from the async fetch (nullopt until fetch completes or when file changes)
    std::optional<FileMetadata> cached_file_metadata_;

    // === Callbacks ===
    DeleteConfirmedCallback on_delete_confirmed_;

    // === Internal Methods ===

    /**
     * @brief Load gcode file for 3D/2D preview
     *
     * Downloads gcode via Moonraker API and loads into gcode_viewer widget.
     * Shows all layers with no ghost (progress = -1). Falls back to thumbnail
     * on failure, disabled config, or oversized files.
     */
    void load_gcode_for_preview();

    /**
     * @brief Show or hide the gcode viewer
     *
     * Sets detail_gcode_viewer_mode_ subject to control XML visibility bindings.
     * @param show true to show viewer, false to revert to thumbnail
     */
    void show_gcode_viewer(bool show);

    /**
     * @brief Apply tool colors to the gcode viewer
     *
     * Reads AMS slot colors when available, falls back to file metadata colors.
     */
    void apply_tool_colors();

    /**
     * @brief Re-apply tool colors from user's filament mapping choices
     */
    void apply_mapped_tool_colors();

    /**
     * @brief Extract filament colors from parsed gcode when metadata lacks them
     *
     * Called after gcode viewer finishes loading. If current_filament_colors_ is empty,
     * extracts tool_color_palette from the parsed file and updates the mapping card.
     * Fixes Snapmaker (and other printers whose Moonraker doesn't return filament_colors).
     */
    void try_extract_gcode_colors(lv_obj_t* viewer);

    /**
     * @brief Populate the dynamic per-printer option-toggle rows.
     *
     * Reads the active printer's `PrePrintOptionSet` from PrinterState and
     * regenerates the rows inside `pre_print_options_container_`. Idempotent
     * — safe to call repeatedly; only rebuilds when the printer type changes.
     * Wires the resulting state subjects through to the prep manager via
     * `set_option_state_provider()`, and binds a single value-changed
     * callback that updates the prep-time estimate.
     */
    void populate_option_rows();

    /**
     * @brief Static callback for delete confirmation
     */
    static void on_confirm_delete_static(lv_event_t* e);

    /**
     * @brief Static callback for cancel delete
     */
    static void on_cancel_delete_static(lv_event_t* e);

    /**
     * @brief Update color swatches display.
     *
     * Renders one swatch per entry in `tool_indices`, sourcing each swatch's
     * color from the live AMS backend slot when available (slot index = tool
     * index), falling back to `palette_colors[tool]` when no backend exists.
     * Also publishes `empty_tools_warning_` (1 if any used tool's slot is
     * empty, 0 otherwise).
     *
     * @param tool_indices Tool indices to render (from
     *                     ParsedGCodeFile::tools_used_indices)
     * @param palette_colors Slicer-provided palette (fallback when no AMS
     *                       backend; indexed by tool)
     */
    void update_color_swatches(const std::set<int>& tool_indices,
                               const std::vector<std::string>& palette_colors);

    /**
     * @brief Whether the FILAMENTS card should be visible for `tool_count` tools.
     *
     * Multi-tool printers (toolchanger / multi-extruder / multi-slot AMS) show
     * the card whenever at least one tool is referenced — lane identity matters
     * even for single-tool prints. Single-extruder printers only show it for
     * 2+ tools (manual-swap multi-color files). Caller is responsible for
     * AND-ing with `!filament_mapping_card_.should_show()`.
     */
    [[nodiscard]] bool swatches_card_visible_for(size_t tool_count) const;
};

} // namespace helix::ui

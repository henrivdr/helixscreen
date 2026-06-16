// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

class PrinterState;
struct ToolInfo;

/// Collapse a tool list to the distinct physical extruders feeding it, preserving
/// first-seen order. Multiplexing backends (AFC BoxTurtle, Happy Hare, ERCF) expose
/// one logical tool per spool lane that all share a single extruder; a true
/// toolchanger maps each tool to its own extruder. The nozzle-temps widget shows
/// one row per physical nozzle, so it iterates this rather than tools() directly.
/// Tools with no extruder_name are dropped.
[[nodiscard]] std::vector<std::string>
distinct_extruder_names(const std::vector<ToolInfo>& tools);

/// Panel widget showing per-extruder temperature rows with progress bars.
/// Gated on show_tool_badge (multi-tool printers only). Displays each
/// extruder's current/target temperature and a colored progress bar,
/// plus a bed temperature row at the bottom.
class NozzleTempsWidget : public PanelWidget {
  public:
    explicit NozzleTempsWidget(PrinterState& printer_state);
    ~NozzleTempsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "nozzle_temps";
    }

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    struct ExtruderRow {
        std::string name;
        std::string short_name; ///< Compact label (e.g. "T0") shown in narrow layouts
        std::string long_name;  ///< Verbose label (e.g. "Nozzle 1") shown when colspan >= 2
        lv_obj_t* row_obj = nullptr;
        lv_obj_t* tool_label = nullptr;
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* target_label = nullptr;
        lv_obj_t* progress_bar = nullptr;
        // Lifetimes MUST be declared before observers: C++ destroys members in
        // reverse order, so observers are destroyed first (calling lv_observer_remove
        // while the lifetime shared_ptr is still alive and the subject is valid).
        // If the subject was already freed (reconnect), clear_rows() explicitly
        // resets lifetimes before observers to let the weak_ptr expire. (#673)
        SubjectLifetime temp_lifetime;
        SubjectLifetime target_lifetime;
        ObserverGuard temp_observer;
        ObserverGuard target_observer;
        int cached_temp = 0;
        int cached_target = 0;
    };

    std::vector<ExtruderRow> extruder_rows_;

    lv_obj_t* bed_row_ = nullptr;
    lv_obj_t* bed_temp_label_ = nullptr;
    lv_obj_t* bed_target_label_ = nullptr;
    lv_obj_t* bed_progress_bar_ = nullptr;
    // Lifetimes MUST be declared before observers (same pattern as ExtruderRow)
    SubjectLifetime bed_temp_lifetime_;
    SubjectLifetime bed_target_lifetime_;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
    int cached_bed_temp_ = 0;
    int cached_bed_target_ = 0;

    ObserverGuard version_observer_;
    int rebuild_gen_ = 0;     // Generation counter to break infinite rebuild cycles (L074)
    bool rebuilding_ = false; // Re-entrancy guard: drain() inside clear_rows() can fire
                              // version_observer_ which calls rebuild_rows() again (#723)
    int current_colspan_ = 1; // Last colspan from on_size_changed; rows pick short/long label off this

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer (per-row in extruder_rows_, bed observers, version_observer_)
    // destructs. Without this, queued observer callbacks captured via
    // tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void rebuild_rows();
    void clear_rows();
    void create_extruder_row(lv_obj_t* container, ExtruderRow& row);
    void create_bed_row(lv_obj_t* container);
    void update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label, lv_obj_t* progress_bar,
                            int temp_deci, int target_deci, bool is_bed);
};

void register_nozzle_temps_widget();

} // namespace helix

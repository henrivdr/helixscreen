// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_temps_widget.h"

#include "ui_overlay_temp_graph.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

void register_nozzle_temps_widget() {
    register_widget_factory("nozzle_temps", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<NozzleTempsWidget>(ps);
    });
}

} // namespace helix

using namespace helix;

NozzleTempsWidget::NozzleTempsWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

NozzleTempsWidget::~NozzleTempsWidget() {
    detach();
}

void NozzleTempsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    rebuild_rows();

    // Observe extruder version changes to rebuild rows when tools are discovered.
    // Capture current version to skip the initial immediate callback — rows already built.
    //
    // NOTE: Do NOT gate this on lifetime_.token(). The lifetime is invalidated on
    // every rebuild_rows() → clear_rows() call, which would make the version observer
    // a one-shot: the first rebuild expires the token, and subsequent version changes
    // are silently ignored (#782). This left the widget with stale rows pointing to
    // freed subjects, crashing in lv_observer_remove during the next clear_rows().
    //
    // Safety is provided by: (1) weak_alive in observe_int_sync context (expires when
    // version_observer_ is reset in detach()), (2) rebuilding_ re-entrancy guard,
    // (3) initial_version skip for the attach-time callback.
    int initial_version = lv_subject_get_int(printer_state_.get_extruder_version_subject());
    version_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
        printer_state_.get_extruder_version_subject(), this,
        [initial_version](NozzleTempsWidget* self, int version) {
            if (version == initial_version)
                return; // Skip initial callback — rows already built in attach()
            self->rebuild_rows();
        });

    spdlog::debug("[NozzleTempsWidget] Attached with {} extruder rows", extruder_rows_.size());
}

void NozzleTempsWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    clear_rows();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::clear_rows() {
    // Invalidate lifetime to expire all pending deferred observer callbacks.
    // This replaces drain() which caused re-entrant process_pending() crashes
    // when SensorState::set_sensors() → drain() → rebuild_rows() → clear_rows()
    // → drain() ran while subjects were in a half-torn-down state (#732).
    // Freeze prevents new callbacks from being queued during cleanup.
    lifetime_.invalidate();
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();

    ++rebuild_gen_;

    // Release lifetime tokens BEFORE destroying observers. The ObserverGuard
    // holds a weak_ptr to the SubjectLifetime shared_ptr. If the dynamic subject
    // was already destroyed (reconnection), the source's shared_ptr is gone —
    // but our row's copy keeps the weak_ptr alive. Resetting our copy first
    // lets the weak_ptr expire, so ObserverGuard::reset() safely skips
    // lv_observer_remove() on the freed observer. (#673, #698)
    for (auto& row : extruder_rows_) {
        row.temp_lifetime.reset();
        row.target_lifetime.reset();
        row.temp_observer.reset();
        row.target_observer.reset();
    }
    extruder_rows_.clear();
    // Bed subjects are also destroyed during deinit_subjects() — need lifetime
    // tokens to prevent lv_observer_remove() on freed subjects (#734)
    bed_temp_lifetime_.reset();
    bed_target_lifetime_.reset();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    auto* container =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container") : nullptr;
    if (container)
        helix::ui::safe_clean_children(container);

    bed_row_ = nullptr;
    bed_temp_label_ = nullptr;
    bed_target_label_ = nullptr;
    bed_progress_bar_ = nullptr;
    cached_bed_temp_ = 0;
    cached_bed_target_ = 0;
}

void NozzleTempsWidget::rebuild_rows() {
    // Guard against re-entrancy from deferred observer callbacks that may
    // trigger version changes during the rebuild cycle (#723, #724, #725).
    if (rebuilding_)
        return;
    rebuilding_ = true;

    clear_rows();

    auto* container =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container") : nullptr;
    if (!container) {
        spdlog::warn("[NozzleTempsWidget] Container not found in XML");
        rebuilding_ = false;
        return;
    }

    auto token = lifetime_.token();

    // Create extruder rows ordered by ToolState tools
    const auto& tools = ToolState::instance().tools();
    for (const auto& tool : tools) {
        if (!tool.extruder_name)
            continue;

        ExtruderRow row;
        row.name = *tool.extruder_name;
        create_extruder_row(container, row);

        // Observe per-extruder temp subject with lifetime token
        lv_subject_t* temp_subj =
            printer_state_.get_extruder_temp_subject(row.name, row.temp_lifetime);
        lv_subject_t* target_subj =
            printer_state_.get_extruder_target_subject(row.name, row.target_lifetime);

        if (temp_subj) {
            row.cached_temp = lv_subject_get_int(temp_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            auto* bar = row.progress_bar;
            row.temp_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                temp_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl,
                 bar](NozzleTempsWidget* self, int temp) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        self->extruder_rows_[idx].cached_temp = temp;
                        self->update_row_display(temp_lbl, target_lbl, bar, temp,
                                                 self->extruder_rows_[idx].cached_target, false);
                    }
                },
                row.temp_lifetime);
        }

        if (target_subj) {
            row.cached_target = lv_subject_get_int(target_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            auto* bar = row.progress_bar;
            row.target_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                target_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl,
                 bar](NozzleTempsWidget* self, int target) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        self->extruder_rows_[idx].cached_target = target;
                        self->update_row_display(temp_lbl, target_lbl, bar,
                                                 self->extruder_rows_[idx].cached_temp, target,
                                                 false);
                    }
                },
                row.target_lifetime);
        }

        // Initial display update
        update_row_display(row.temp_label, row.target_label, row.progress_bar, row.cached_temp,
                           row.cached_target, false);

        extruder_rows_.push_back(std::move(row));
    }

    // Bed row at the end
    create_bed_row(container);

    // Bed subjects are destroyed during deinit_subjects() — use lifetime tokens (#734)
    lv_subject_t* bed_temp_subj = printer_state_.get_bed_temp_subject(bed_temp_lifetime_);
    lv_subject_t* bed_target_subj = printer_state_.get_bed_target_subject(bed_target_lifetime_);

    if (bed_temp_subj) {
        cached_bed_temp_ = lv_subject_get_int(bed_temp_subj);
        bed_temp_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_temp_subj, this,
            [token](NozzleTempsWidget* self, int temp) {
                if (token.expired())
                    return;
                self->cached_bed_temp_ = temp;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_,
                                         self->bed_progress_bar_, temp, self->cached_bed_target_,
                                         true);
            },
            bed_temp_lifetime_);
    }

    if (bed_target_subj) {
        cached_bed_target_ = lv_subject_get_int(bed_target_subj);
        bed_target_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_target_subj, this,
            [token](NozzleTempsWidget* self, int target) {
                if (token.expired())
                    return;
                self->cached_bed_target_ = target;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_,
                                         self->bed_progress_bar_, self->cached_bed_temp_, target,
                                         true);
            },
            bed_target_lifetime_);
    }

    update_row_display(bed_temp_label_, bed_target_label_, bed_progress_bar_, cached_bed_temp_,
                       cached_bed_target_, true);

    rebuilding_ = false;
    spdlog::debug("[NozzleTempsWidget] Rebuilt with {} extruder rows + bed", extruder_rows_.size());
}

void NozzleTempsWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                        int /*height_px*/) {
    if (!widget_obj_)
        return;

    auto* container = lv_obj_find_by_name(widget_obj_, "nozzle_temps_container");
    if (!container)
        return;

    current_colspan_ = colspan;

    // Wide layout (2x1): use row-wrap so items flow into 2 columns
    bool wide = (colspan >= 2 && rowspan <= 1);
    if (wide) {
        lv_obj_set_style_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP, 0);
        lv_obj_set_style_pad_column(container, theme_manager_get_spacing("space_md"), 0);
        lv_obj_set_style_flex_main_place(container, LV_FLEX_ALIGN_SPACE_EVENLY, 0);
        // Each row takes just under half so two fit per line
        for (auto& row : extruder_rows_) {
            if (row.row_obj)
                lv_obj_set_width(row.row_obj, lv_pct(45));
        }
        if (bed_row_) {
            lv_obj_set_width(bed_row_, lv_pct(45));
            // Remove divider border + its top padding in wide layout
            lv_obj_set_style_border_width(bed_row_, 0, 0);
            lv_obj_set_style_pad_top(bed_row_, 0, 0);
        }
    } else {
        lv_obj_set_style_flex_flow(container, LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_set_style_pad_column(container, 0, 0);
        lv_obj_set_style_flex_main_place(container, LV_FLEX_ALIGN_START, 0);
        for (auto& row : extruder_rows_) {
            if (row.row_obj)
                lv_obj_set_width(row.row_obj, lv_pct(100));
        }
        if (bed_row_) {
            lv_obj_set_width(bed_row_, lv_pct(100));
            lv_obj_set_style_border_width(bed_row_, 1, 0);
            lv_obj_set_style_pad_top(bed_row_, theme_manager_get_spacing("space_xxs"), 0);
        }
    }

    // Use compact font when widget is narrow (single column)
    const lv_font_t* text_font = (colspan <= 1) ? theme_manager_get_font("font_xs") : nullptr;
    if (text_font) {
        auto set_font = [text_font](lv_obj_t* lbl) {
            if (lbl)
                lv_obj_set_style_text_font(lbl, text_font, LV_PART_MAIN);
        };
        for (auto& row : extruder_rows_) {
            set_font(row.temp_label);
            set_font(row.target_label);
        }
        set_font(bed_temp_label_);
        set_font(bed_target_label_);
    }

    // Swap row labels between short ("T0") and long ("Nozzle 1") based on the
    // available horizontal room.
    for (auto& row : extruder_rows_) {
        if (!row.tool_label)
            continue;
        const std::string& text = (colspan >= 2) ? row.long_name : row.short_name;
        lv_label_set_text(row.tool_label, text.c_str());
    }

    spdlog::debug("[NozzleTempsWidget] on_size_changed {}x{} wide={}", colspan, rowspan, wide);
}

void NozzleTempsWidget::create_extruder_row(lv_obj_t* container, ExtruderRow& row) {
    // Short label: the tool identifier (e.g. "T0"); falls back to the klipper
    // extruder name when no tool is mapped (multi-extruder, no toolchanger).
    std::string short_name = ToolState::instance().tool_name_for_extruder(row.name);
    if (short_name.empty())
        short_name = row.name;

    // Long label: prefer the user-friendly "Nozzle N" from PrinterTemperatureState
    // when the tool identifier is just the default Tn pattern. For toolchangers
    // with viesturz-named tools (e.g. "Left", "Right"), the configured tool name
    // is already meaningful — keep it.
    std::string long_name = short_name;
    bool is_default_tn = short_name.size() >= 2 && short_name[0] == 'T' &&
                         std::all_of(short_name.begin() + 1, short_name.end(),
                                     [](char c) {
                                         return std::isdigit(static_cast<unsigned char>(c));
                                     });
    if (is_default_tn) {
        const auto& exts = printer_state_.temperature_state().extruders();
        auto it = exts.find(row.name);
        if (it != exts.end() && !it->second.display_name.empty())
            long_name = it->second.display_name;
    }

    row.short_name = std::move(short_name);
    row.long_name = std::move(long_name);

    const std::string& initial_label =
        (current_colspan_ >= 2) ? row.long_name : row.short_name;

    // Create row from XML template — layout, fonts, colors are all declarative
    const char* attrs[] = {"tool_name", initial_label.c_str(), nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_row", attrs));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_row') returned NULL for '{}'",
                      row.name);
        return;
    }

    row.row_obj = row_obj;
    row.tool_label = lv_obj_find_by_name(row_obj, "tool_label");
    row.temp_label = lv_obj_find_by_name(row_obj, "temp_label");
    row.target_label = lv_obj_find_by_name(row_obj, "target_label");
    row.progress_bar = lv_obj_find_by_name(row_obj, "progress_bar");

    // Tap row → open nozzle temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::create_bed_row(lv_obj_t* container) {
    // Create bed row from XML template — divider, layout, colors are all declarative
    lv_obj_t* row_obj =
        static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_bed_row", nullptr));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_bed_row') returned NULL");
        return;
    }

    bed_row_ = row_obj;
    bed_temp_label_ = lv_obj_find_by_name(row_obj, "bed_temp_label");
    bed_target_label_ = lv_obj_find_by_name(row_obj, "bed_target_label");
    bed_progress_bar_ = lv_obj_find_by_name(row_obj, "bed_progress_bar");

    // Tap bed row → open bed temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label,
                                           lv_obj_t* /* progress_bar */, int temp_centi,
                                           int target_centi, bool /* is_bed */) {
    if (!temp_label || !target_label)
        return;

    auto result = helix::ui::temperature::heater_display(temp_centi, target_centi);

    // Current temp with color coding (green=at-temp, red=heating, blue=cooling, gray=off)
    lv_label_set_text_fmt(temp_label, "%d\xC2\xB0", temp_centi / 10);
    lv_obj_set_style_text_color(temp_label, result.color, LV_PART_MAIN);

    if (target_centi > 0) {
        lv_label_set_text_fmt(target_label, "/ %d\xC2\xB0", target_centi / 10);
    } else {
        lv_label_set_text(target_label, lv_tr("off"));
    }
}

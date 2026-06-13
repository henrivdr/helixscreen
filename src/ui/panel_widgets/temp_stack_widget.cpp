// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_temp_display.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_service.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_temp_stack_widget() {
    register_widget_factory("temp_stack", [](const std::string&) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TemperatureService>();
        return std::make_unique<TempStackWidget>(ps, tcp);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "temp_stack_nozzle_cb",
                             TempStackWidget::temp_stack_nozzle_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_bed_cb", TempStackWidget::temp_stack_bed_cb);
    lv_xml_register_event_cb(nullptr, "temp_stack_chamber_cb",
                             TempStackWidget::temp_stack_chamber_cb);
    lv_xml_register_event_cb(nullptr, "temp_carousel_page_cb",
                             TempStackWidget::temp_carousel_page_cb);
}
} // namespace helix

namespace {
// Make all children of a page pass events through (not clickable, bubble to parent)
void make_children_passthrough(lv_obj_t* parent) {
    if (!parent)
        return;
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (!child)
            continue;
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE);
        make_children_passthrough(child);
    }
}
} // namespace

using namespace helix;

// Static instance pointer for callback dispatch (only one temp_stack widget at a time)
static TempStackWidget* s_active_instance = nullptr;

TempStackWidget::TempStackWidget(PrinterState& printer_state, TemperatureService* temp_panel)
    : printer_state_(printer_state), temp_control_panel_(temp_panel) {}

TempStackWidget::~TempStackWidget() {
    detach();
}

void TempStackWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

std::string TempStackWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_temp_carousel";
    }
    return "panel_widget_temp_stack";
}

bool TempStackWidget::on_edit_configure() {
    bool was_carousel = is_carousel_mode();
    nlohmann::json new_config = config_;
    if (was_carousel) {
        new_config.erase("display_mode");
    } else {
        new_config["display_mode"] = "carousel";
    }
    spdlog::info("[TempStackWidget] Toggling display_mode: {} → {}",
                 was_carousel ? "carousel" : "stack", was_carousel ? "stack" : "carousel");
    save_widget_config(new_config);
    return true;
}

bool TempStackWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void TempStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_active_instance = this;
    lv_obj_set_user_data(widget_obj_, this);

    // Pressed feedback: dim each clickable row on touch
    for (const char* name :
         {"temp_stack_nozzle_row", "temp_stack_bed_row", "temp_stack_chamber_row"}) {
        lv_obj_t* row = lv_obj_find_by_name(widget_obj_, name);
        if (row)
            lv_obj_set_style_opa(row, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    if (is_carousel_mode()) {
        attach_carousel(widget_obj);
    } else {
        attach_stack(widget_obj);
    }
}

void TempStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    using helix::ui::observe_int_sync;
    auto token = lifetime_.token();

    // Nozzle observers
    nozzle_temp_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_temp_subject(), this,
                                          [token](TempStackWidget* self, int temp) {
                                              if (token.expired())
                                                  return;
                                              self->on_nozzle_temp_changed(temp);
                                          });
    nozzle_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_target_subject(), this,
                                          [token](TempStackWidget* self, int target) {
                                              if (token.expired())
                                                  return;
                                              self->on_nozzle_target_changed(target);
                                          });

    // Bed observers (subjects destroyed during deinit — need lifetime tokens #734)
    bed_temp_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_temp_subject(bed_temp_lifetime_), this,
        [token](TempStackWidget* self, int temp) {
            if (token.expired())
                return;
            self->on_bed_temp_changed(temp);
        },
        bed_temp_lifetime_);
    bed_target_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_target_subject(bed_target_lifetime_), this,
        [token](TempStackWidget* self, int target) {
            if (token.expired())
                return;
            self->on_bed_target_changed(target);
        },
        bed_target_lifetime_);

    // Attach nozzle animator - look for the glyph inside the nozzle_icon component
    lv_obj_t* nozzle_icon = lv_obj_find_by_name(widget_obj_, "nozzle_icon_glyph");
    if (nozzle_icon) {
        nozzle_animator_.attach(nozzle_icon);
        cached_nozzle_temp_ = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        cached_nozzle_target_ =
            lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
    }

    // Attach bed animator
    lv_obj_t* bed_icon = lv_obj_find_by_name(widget_obj_, "temp_stack_bed_icon_glyph");
    if (bed_icon) {
        bed_animator_.attach(bed_icon);
        cached_bed_temp_ = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        cached_bed_target_ = lv_subject_get_int(printer_state_.get_bed_target_subject());
        bed_animator_.update(cached_bed_temp_, cached_bed_target_);
    }

    spdlog::debug("[TempStackWidget] Attached stack with {} animators",
                  (nozzle_icon ? 1 : 0) + (bed_icon ? 1 : 0));
}

void TempStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "temp_carousel");
    if (!carousel) {
        spdlog::error("[TempStackWidget] Could not find temp_carousel in XML");
        return;
    }

    // Use carousel itself as temporary parent (ui_carousel_add_item reparents into tiles)
    lv_obj_t* page_parent = carousel;

    // Helper to create a carousel page with icon + temp_display
    auto create_temp_page = [&](const char* icon_src, const char* icon_name,
                                const char* bind_current, const char* bind_target,
                                const char* page_name) -> lv_obj_t* {
        // Create page container
        lv_obj_t* page = lv_obj_create(page_parent); // reparented by ui_carousel_add_item
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(page, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_name(page, page_name);

        // Click callback to open temp overlay
        lv_obj_add_event_cb(page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);

        // Icon
        const char* icon_attrs[] = {"src",       icon_src, "size",    "md",   "variant",
                                    "secondary", "name",   icon_name, nullptr};
        lv_xml_create(page, "icon", icon_attrs);

        // Temp display (larger, with target shown)
        const char* td_attrs[] = {"size",        "md",           "show_target",
                                  "true",        "bind_current", bind_current,
                                  "bind_target", bind_target,    nullptr};
        lv_xml_create(page, "temp_display", td_attrs);

        // Make children pass events through to the page (clicks + long-press)
        make_children_passthrough(page);

        return page;
    };

    // Nozzle page (use nozzle_icon component for the icon instead)
    lv_obj_t* nozzle_page = lv_obj_create(page_parent);
    lv_obj_set_size(nozzle_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(nozzle_page, 0, 0);
    lv_obj_set_style_bg_opa(nozzle_page, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(nozzle_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(nozzle_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nozzle_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(nozzle_page, theme_manager_get_spacing("space_xs"), 0);
    lv_obj_add_flag(nozzle_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_name(nozzle_page, "nozzle");
    lv_obj_add_event_cb(nozzle_page, temp_carousel_page_cb, LV_EVENT_CLICKED, nullptr);

    const char* nozzle_icon_attrs[] = {
        "size", "md", "badge_subject", "", "name", "carousel_nozzle_icon", nullptr};
    lv_xml_create(nozzle_page, "nozzle_icon", nozzle_icon_attrs);

    const char* nozzle_td_attrs[] = {
        "size",          "md",          "show_target",     "true", "bind_current",
        "extruder_temp", "bind_target", "extruder_target", nullptr};
    lv_xml_create(nozzle_page, "temp_display", nozzle_td_attrs);
    make_children_passthrough(nozzle_page);
    ui_carousel_add_item(carousel, nozzle_page);

    // Attach nozzle heating animator
    lv_obj_t* nozzle_glyph = lv_obj_find_by_name(nozzle_page, "nozzle_icon_glyph");
    if (nozzle_glyph) {
        nozzle_animator_.attach(nozzle_glyph);
        cached_nozzle_temp_ = lv_subject_get_int(printer_state_.get_active_extruder_temp_subject());
        cached_nozzle_target_ =
            lv_subject_get_int(printer_state_.get_active_extruder_target_subject());
        nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
    }

    // Bed page
    lv_obj_t* bed_page =
        create_temp_page("radiator", "carousel_bed_icon", "bed_temp", "bed_target", "bed");
    ui_carousel_add_item(carousel, bed_page);

    // Attach bed heating animator
    lv_obj_t* bed_glyph = lv_obj_find_by_name(bed_page, "carousel_bed_icon");
    if (bed_glyph) {
        // The icon component wraps a glyph child — try to find the actual glyph
        lv_obj_t* inner_glyph = lv_obj_get_child(bed_glyph, 0);
        if (inner_glyph) {
            bed_animator_.attach(inner_glyph);
        } else {
            bed_animator_.attach(bed_glyph);
        }
        cached_bed_temp_ = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        cached_bed_target_ = lv_subject_get_int(printer_state_.get_bed_target_subject());
        bed_animator_.update(cached_bed_temp_, cached_bed_target_);
    }

    // Chamber page (only if sensor present)
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        lv_obj_t* chamber_page = create_temp_page("fridge_industrial", "carousel_chamber_icon",
                                                  "chamber_temp", "chamber_temp", "chamber");
        ui_carousel_add_item(carousel, chamber_page);
    }

    // Observe heating state for animators in carousel mode
    using helix::ui::observe_int_sync;
    auto token = lifetime_.token();

    nozzle_temp_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_temp_subject(), this,
                                          [token](TempStackWidget* self, int temp) {
                                              if (token.expired())
                                                  return;
                                              self->on_nozzle_temp_changed(temp);
                                          });
    nozzle_target_observer_ =
        observe_int_sync<TempStackWidget>(printer_state_.get_active_extruder_target_subject(), this,
                                          [token](TempStackWidget* self, int target) {
                                              if (token.expired())
                                                  return;
                                              self->on_nozzle_target_changed(target);
                                          });
    bed_temp_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_temp_subject(bed_temp_lifetime_), this,
        [token](TempStackWidget* self, int temp) {
            if (token.expired())
                return;
            self->on_bed_temp_changed(temp);
        },
        bed_temp_lifetime_);
    bed_target_observer_ = observe_int_sync<TempStackWidget>(
        printer_state_.get_bed_target_subject(bed_target_lifetime_), this,
        [token](TempStackWidget* self, int target) {
            if (token.expired())
                return;
            self->on_bed_target_changed(target);
        },
        bed_target_lifetime_);

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[TempStackWidget] Attached carousel with {} pages", page_count);
}

void TempStackWidget::on_size_changed(int /*colspan*/, int rowspan, int /*width_px*/,
                                      int /*height_px*/) {
    if (!widget_obj_ || is_carousel_mode())
        return;

    // Determine size tier based on vertical space
    const char* size = (rowspan >= 2) ? "sm" : "xs";

    // Get text font for this size tier
    const char* font_token = theme_manager_size_to_font_token(size, "xs");
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    // Icon font: xs=16px, sm=24px
    const lv_font_t* icon_font = (rowspan >= 2) ? &mdi_icons_24 : &mdi_icons_16;

    // Update nozzle icon glyph
    lv_obj_t* nozzle_glyph = lv_obj_find_by_name(widget_obj_, "nozzle_icon_glyph");
    if (nozzle_glyph)
        lv_obj_set_style_text_font(nozzle_glyph, icon_font, 0);

    // Update bed icon — the <icon> component wraps a label child
    lv_obj_t* bed_icon = lv_obj_find_by_name(widget_obj_, "temp_stack_bed_icon_glyph");
    if (bed_icon) {
        lv_obj_t* glyph = lv_obj_get_child(bed_icon, 0);
        if (glyph)
            lv_obj_set_style_text_font(glyph, icon_font, 0);
    }

    // Update temp_display fonts and icon fonts in all rows
    const char* row_names[] = {"temp_stack_nozzle_row", "temp_stack_bed_row",
                               "temp_stack_chamber_row"};
    for (const char* row_name : row_names) {
        lv_obj_t* row = lv_obj_find_by_name(widget_obj_, row_name);
        if (!row)
            continue;
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
            if (ui_temp_display_is_valid(child)) {
                // Update all labels inside the temp_display container
                for (uint32_t j = 0; j < lv_obj_get_child_count(child); j++) {
                    lv_obj_t* label = lv_obj_get_child(child, static_cast<int32_t>(j));
                    lv_obj_set_style_text_font(label, text_font, 0);
                }
            } else if (lv_obj_get_child_count(child) > 0) {
                // Icon component — update first child glyph (chamber row icon)
                lv_obj_t* glyph = lv_obj_get_child(child, 0);
                if (glyph)
                    lv_obj_set_style_text_font(glyph, icon_font, 0);
            }
        }
    }

    spdlog::debug("[TempStackWidget] on_size_changed rowspan={} -> size {}", rowspan, size);
}

void TempStackWidget::detach() {
    lifetime_.invalidate();
    nozzle_animator_.detach();
    bed_animator_.detach();
    nozzle_temp_observer_.reset();
    nozzle_target_observer_.reset();
    bed_temp_lifetime_.reset();
    bed_target_lifetime_.reset();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }

    if (widget_obj_)
        lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempStackWidget] Detached");
}

void TempStackWidget::on_nozzle_temp_changed(int temp_deci) {
    cached_nozzle_temp_ = temp_deci;
    nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
}

void TempStackWidget::on_nozzle_target_changed(int target_deci) {
    cached_nozzle_target_ = target_deci;
    nozzle_animator_.update(cached_nozzle_temp_, cached_nozzle_target_);
}

void TempStackWidget::on_bed_temp_changed(int temp_deci) {
    cached_bed_temp_ = temp_deci;
    bed_animator_.update(cached_bed_temp_, cached_bed_target_);
}

void TempStackWidget::on_bed_target_changed(int target_deci) {
    cached_bed_target_ = target_deci;
    bed_animator_.update(cached_bed_temp_, cached_bed_target_);
}

void TempStackWidget::handle_nozzle_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Nozzle click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Nozzle clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void TempStackWidget::handle_bed_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Bed click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Bed clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

void TempStackWidget::handle_chamber_clicked() {
    if (long_pressed_) {
        long_pressed_ = false;
        spdlog::debug("[TempStackWidget] Chamber click suppressed (follows long-press)");
        return;
    }

    spdlog::info("[TempStackWidget] Chamber clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Chamber, parent_screen_);
}

void TempStackWidget::temp_stack_nozzle_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_nozzle_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_nozzle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_bed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_bed_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_bed_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_stack_chamber_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_stack_chamber_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_chamber_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void TempStackWidget::temp_carousel_page_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[TempStackWidget] temp_carousel_page_cb");
    if (s_active_instance) {
        auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        const char* page_id = lv_obj_get_name(target);
        if (page_id) {
            if (std::strcmp(page_id, "nozzle") == 0) {
                s_active_instance->handle_nozzle_clicked();
            } else if (std::strcmp(page_id, "bed") == 0) {
                s_active_instance->handle_bed_clicked();
            } else if (std::strcmp(page_id, "chamber") == 0) {
                s_active_instance->handle_chamber_clicked();
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

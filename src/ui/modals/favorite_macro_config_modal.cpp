// SPDX-License-Identifier: GPL-3.0-or-later
#include "favorite_macro_config_modal.h"

#include "ui_event_safety.h"

#include "favorite_macro_config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {
lv_subject_t s_tab_subject;
lv_subject_t s_skip_subject;
bool s_subjects_registered = false;
} // namespace

FavoriteMacroConfigModal* FavoriteMacroConfigModal::s_active_ = nullptr;

void register_favorite_macro_config_subjects() {
    if (s_subjects_registered)
        return;
    lv_subject_init_int(&s_tab_subject, 0);
    lv_subject_init_int(&s_skip_subject, 0);
    lv_xml_register_subject(nullptr, "fav_macro_config_tab", &s_tab_subject);
    lv_xml_register_subject(nullptr, "fav_macro_skip_params", &s_skip_subject);
    s_subjects_registered = true;
}

FavoriteMacroConfigModal::FavoriteMacroConfigModal(const std::string& widget_id,
                                                   const std::string& panel_id,
                                                   ChangeCallback on_change)
    : widget_id_(widget_id), panel_id_(panel_id), on_change_(std::move(on_change)) {
    s_active_ = this;
}

FavoriteMacroConfigModal::~FavoriteMacroConfigModal() {
    if (s_active_ == this)
        s_active_ = nullptr;
}

void FavoriteMacroConfigModal::load_config() {
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    auto c = favorite_macro_config_from_json(wc.get_widget_config(widget_id_));
    macro_name_ = c.macro;
    icon_name_ = c.icon;
    icon_color_ = c.color;
    skip_param_prompt_ = c.skip_param_prompt;
}

void FavoriteMacroConfigModal::persist() {
    auto& wc = PanelWidgetManager::instance().get_widget_config(panel_id_);
    FavoriteMacroConfig c{macro_name_, icon_name_, icon_color_, skip_param_prompt_};
    wc.set_widget_config(widget_id_, favorite_macro_config_to_json(c));
    if (on_change_)
        on_change_();
}

void FavoriteMacroConfigModal::on_show() {
    if (dialog())
        lv_obj_set_user_data(dialog(), this);
    load_config();
    macro_list_ = lv_obj_find_by_name(dialog(), "macro_list");
    icon_grid_ = lv_obj_find_by_name(dialog(), "icon_grid");
    color_grid_ = lv_obj_find_by_name(dialog(), "color_grid");
    lv_subject_set_int(&s_tab_subject, 0);
    lv_subject_set_int(&s_skip_subject, skip_param_prompt_ ? 1 : 0);
    populate_macro_list();
    populate_icon_grid();
    populate_color_grid();
    spdlog::debug("[FavoriteMacroConfig] Opened: widget={}, macro={}", widget_id_, macro_name_);
}

void FavoriteMacroConfigModal::tab_macro_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_macro_cb");
    lv_subject_set_int(&s_tab_subject, 0);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::tab_appearance_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_appearance_cb");
    lv_subject_set_int(&s_tab_subject, 1);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::tab_options_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] tab_options_cb");
    lv_subject_set_int(&s_tab_subject, 2);
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::close_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] close_cb");
    if (s_active_)
        s_active_->hide();
    LVGL_SAFE_EVENT_CB_END();
}

void FavoriteMacroConfigModal::skip_params_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FavoriteMacroConfigModal] skip_params_cb");
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (s_active_)
        s_active_->select_skip_param(lv_obj_has_state(sw, LV_STATE_CHECKED));
    LVGL_SAFE_EVENT_CB_END();
}

// Empty stubs until Task 3.
void FavoriteMacroConfigModal::populate_macro_list() {}
void FavoriteMacroConfigModal::populate_icon_grid() {}
void FavoriteMacroConfigModal::populate_color_grid() {}
void FavoriteMacroConfigModal::refresh_highlights() {}
void FavoriteMacroConfigModal::select_macro(const std::string&) {}
void FavoriteMacroConfigModal::select_icon(const std::string&) {}
void FavoriteMacroConfigModal::select_color(uint32_t) {}
void FavoriteMacroConfigModal::select_skip_param(bool enabled) {
    skip_param_prompt_ = enabled;
    persist();
}
MoonrakerAPI* FavoriteMacroConfigModal::get_api() const {
    return nullptr;
}
void FavoriteMacroConfigModal::macro_row_cb(lv_event_t*) {}
void FavoriteMacroConfigModal::icon_cell_cb(lv_event_t*) {}
void FavoriteMacroConfigModal::color_swatch_cb(lv_event_t*) {}

} // namespace helix

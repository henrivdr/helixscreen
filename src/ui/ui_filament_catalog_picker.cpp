// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_filament_catalog_picker.h"

#include "ui_icon_codepoints.h"
#include "ui_utils.h"

#include "lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix::ui {

bool FilamentCatalogPickerModal::callbacks_registered_ = false;
FilamentCatalogPickerModal* FilamentCatalogPickerModal::active_instance_ = nullptr;

void FilamentCatalogPickerModal::register_callbacks() {
    if (callbacks_registered_)
        return;
    lv_xml_register_event_cb(nullptr, "catalog_picker_close_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->hide();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_vendor_changed_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->handle_vendor_changed();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_type_changed_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->handle_type_changed();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_select_cb", [](lv_event_t*) {
        if (active_instance_)
            active_instance_->handle_select_button();
    });
    lv_xml_register_event_cb(nullptr, "catalog_picker_reset_cb", [](lv_event_t*) {
        if (!active_instance_)
            return;
        FilamentCatalogPickerModal* self = active_instance_;
        if (self->reset_callback_)
            self->reset_callback_();
        self->hide();
    });
    callbacks_registered_ = true;
}

void FilamentCatalogPickerModal::set_reset_callback(std::function<void()> cb) {
    reset_callback_ = std::move(cb);
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      SelectCallback on_select) {
    show(parent, std::move(seed_type), std::nullopt, std::move(on_select));
}

void FilamentCatalogPickerModal::show(lv_obj_t* parent, std::optional<std::string> seed_type,
                                      std::optional<std::vector<std::string>> allowed_types,
                                      SelectCallback on_select) {
    register_callbacks();
    catalog_ = helix::printer::FilamentCatalog::load_full(); // fresh load per open
    seed_type_ = std::move(seed_type);
    allowed_types_ = std::move(allowed_types);
    on_select_ = std::move(on_select);
    highlighted_id_.clear();
    Modal::show(parent); // triggers on_show()
}

void FilamentCatalogPickerModal::on_show() {
    active_instance_ = this;
    populate_vendor_dropdown();
    populate_type_dropdown();
    rebuild_product_list();
    apply_input_surface();

    // "Reset to defaults" is the leading (tertiary) action in the button row; gate it and
    // its divider on whether a reset callback was set before show() (preset-editing
    // context) — same gate the retired MaterialPickerMenu used.
    const bool show_reset = (reset_callback_ != nullptr);
    for (const char* n : {"btn_tertiary", "div_tertiary"}) {
        lv_obj_t* o = lv_obj_find_by_name(dialog(), n);
        if (!o)
            continue;
        if (show_reset) {
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FilamentCatalogPickerModal::apply_input_surface() {
    if (!dialog())
        return;
    // The theme's dialog-input contrast (dropdowns adopt overlay_bg on an elevated dialog
    // surface) does not land here: theme_apply_current_palette_to_tree() runs inside
    // Modal::show *before* the picker subtree is fully assembled, so is_on_elevated_surface()
    // can't yet see the USER_1 dialog ancestor and leaves both dropdowns (and their popup
    // lists) at elevated_bg — the exact color of the dialog. By on_show the tree is stable,
    // so re-assert the darker input surface here via the theme token (stays theme-reactive).
    // The dropdown's list is created in its constructor and persists across open/close, so
    // styling it now sticks. Mirrors BufferStatusModal's post-reapply fixup.
    lv_color_t input_bg = theme_manager_get_color("overlay_bg");
    for (const char* n : {"vendor_dropdown", "type_dropdown"}) {
        lv_obj_t* dd = lv_obj_find_by_name(dialog(), n);
        if (!dd)
            continue;
        lv_obj_set_style_bg_color(dd, input_bg, LV_PART_MAIN);
        if (lv_obj_t* list = lv_dropdown_get_list(dd))
            lv_obj_set_style_bg_color(list, input_bg, LV_PART_MAIN);
    }
}

void FilamentCatalogPickerModal::on_hide() {
    if (active_instance_ == this)
        active_instance_ = nullptr;
    catalog_ =
        helix::printer::FilamentCatalog{}; // free the 356-product catalog; reloaded on next show()
}

std::string FilamentCatalogPickerModal::current_vendor() const {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "vendor_dropdown");
    if (!dd)
        return {};
    uint32_t sel = lv_dropdown_get_selected(dd);
    return sel < vendor_order_.size() ? vendor_order_[sel] : std::string{};
}

std::string FilamentCatalogPickerModal::current_type() const {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "type_dropdown");
    if (!dd)
        return {};
    char buf[64] = {};
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    return buf;
}

void FilamentCatalogPickerModal::populate_vendor_dropdown() {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "vendor_dropdown");
    if (!dd)
        return;
    // "Generic" pinned first, then the rest (all_brands() is already sorted+deduped).
    vendor_order_.clear();
    vendor_order_.push_back("Generic");
    for (const auto& b : catalog_.all_brands()) {
        if (b != "Generic")
            vendor_order_.push_back(b);
    }
    std::string options;
    for (size_t i = 0; i < vendor_order_.size(); ++i) {
        if (i)
            options += "\n";
        options += vendor_order_[i];
    }
    lv_dropdown_set_options(dd, options.c_str());
    lv_dropdown_set_selected(dd, 0); // Generic
}

void FilamentCatalogPickerModal::populate_type_dropdown() {
    lv_obj_t* dd = lv_obj_find_by_name(dialog(), "type_dropdown");
    if (!dd)
        return;
    std::vector<std::string> types = catalog_.types_for_brand(current_vendor());
    if (allowed_types_) {
        std::vector<std::string> filtered;
        for (const auto& t : types) {
            if (std::find(allowed_types_->begin(), allowed_types_->end(), t) !=
                allowed_types_->end())
                filtered.push_back(t);
        }
        types.swap(filtered);
    }
    std::string options;
    int seed_idx = 0;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i)
            options += "\n";
        options += types[i];
        if (seed_type_ && types[i] == *seed_type_)
            seed_idx = static_cast<int>(i);
    }
    lv_dropdown_set_options(dd, options.empty() ? "" : options.c_str());
    lv_dropdown_set_selected(dd, seed_idx);
}

void FilamentCatalogPickerModal::rebuild_product_list() {
    lv_obj_t* list = lv_obj_find_by_name(dialog(), "product_list");
    if (!list)
        return;
    helix::ui::safe_clean_children(list);

    lv_color_t accent = theme_manager_get_color("primary");
    lv_color_t text_color = theme_manager_get_color("text");
    const char* body_font_name = lv_xml_get_const(nullptr, "font_body");
    const lv_font_t* body_font =
        body_font_name ? lv_xml_get_font(nullptr, body_font_name) : lv_font_get_default();
    const char* icon_font_name = lv_xml_get_const(nullptr, "icon_font_xs");
    const lv_font_t* icon_font =
        icon_font_name ? lv_xml_get_font(nullptr, icon_font_name) : body_font;
    const char* check_codepoint = ui_icon::lookup_codepoint("check");

    auto products = catalog_.products_for(current_vendor(), current_type());
    for (const auto* p : products) {
        bool is_current = (highlighted_id_ == p->id);
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, accent, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_name(row, p->id.c_str()); // identity for click handler (L069)

        lv_obj_t* indicator = lv_label_create(row);
        lv_obj_set_style_text_font(indicator, icon_font, 0);
        lv_obj_set_style_min_width(indicator, 16, 0);
        if (is_current && check_codepoint) {
            lv_label_set_text(indicator, check_codepoint);
            lv_obj_set_style_text_color(indicator, accent, 0);
        } else {
            lv_label_set_text(indicator, "");
        }
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, p->name.c_str());
        lv_obj_set_style_text_font(name_lbl, body_font, 0);
        lv_obj_set_style_text_color(name_lbl, is_current ? accent : text_color, 0);
        lv_obj_set_flex_grow(name_lbl, 1);
        lv_obj_remove_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

        char temps[32];
        snprintf(temps, sizeof(temps), "%d\xC2\xB0 / %d\xC2\xB0", p->nozzle_recommended,
                 p->bed_temp);
        lv_obj_t* temp_lbl = lv_label_create(row);
        lv_label_set_text(temp_lbl, temps);
        lv_obj_set_style_text_font(temp_lbl, body_font, 0);
        lv_obj_set_style_text_color(temp_lbl, text_color, 0);
        lv_obj_remove_flag(temp_lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                lv_obj_t* row = lv_event_get_current_target_obj(e);
                if (!row || !active_instance_)
                    return;
                const char* id = lv_obj_get_name(row);
                if (id)
                    active_instance_->handle_row_selected(id);
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void FilamentCatalogPickerModal::handle_vendor_changed() {
    highlighted_id_.clear();  // stale row no longer visible under the new vendor
    populate_type_dropdown(); // vendor changed -> types change
    rebuild_product_list();
}

void FilamentCatalogPickerModal::handle_type_changed() {
    highlighted_id_.clear(); // stale row no longer visible under the new type
    rebuild_product_list();
}

void FilamentCatalogPickerModal::handle_row_selected(const std::string& product_id) {
    highlighted_id_ = product_id;
    rebuild_product_list(); // redraw to move the checkmark
}

void FilamentCatalogPickerModal::handle_select_button() {
    if (highlighted_id_.empty()) {
        spdlog::debug("[FilamentCatalogPicker] Select pressed with no product highlighted");
        return; // no-op; user must pick a row first
    }
    const helix::printer::EffectiveFilament* ef = catalog_.resolve_id(highlighted_id_);
    if (ef && on_select_) {
        helix::printer::EffectiveFilament copy =
            *ef; // by value — nothing dangles after catalog_ reloads
        on_select_(copy);
    }
    hide();
}

} // namespace helix::ui

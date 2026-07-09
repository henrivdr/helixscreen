// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_filament_catalog_picker.h"

#include "lvgl.h"

namespace helix::ui {

// Test-only accessor for FilamentCatalogPickerModal's private selection state.
struct FilamentPickerTestAccess {
    // Highlights the first product in the currently selected vendor+type, as if the
    // user had tapped that row.
    static void select_first_product(FilamentCatalogPickerModal& m) {
        auto products = m.catalog_.products_for(m.current_vendor(), m.current_type());
        if (products.empty()) return;
        m.highlighted_id_ = products.front()->id;
        m.rebuild_product_list();
    }

    static void press_select(FilamentCatalogPickerModal& m) {
        m.handle_select_button();
    }

    // Drives the vendor dropdown to `index` and fires the same change handler the XML
    // "value_changed" event would trigger (types/list rebuild + highlighted_id_ reset).
    static void change_vendor(FilamentCatalogPickerModal& m, uint32_t index) {
        lv_obj_t* dd = lv_obj_find_by_name(m.dialog(), "vendor_dropdown");
        if (!dd) return;
        lv_dropdown_set_selected(dd, index);
        m.handle_vendor_changed();
    }

    static const std::string& highlighted_id(const FilamentCatalogPickerModal& m) {
        return m.highlighted_id_;
    }

    // Returns true if the "Reset to defaults" affordance is currently hidden. The reset
    // action is the leading (tertiary) button in modal_button_row, so its widget is
    // "btn_tertiary" — on_show() toggles its HIDDEN flag on whether a reset callback was set.
    static bool reset_button_hidden(const FilamentCatalogPickerModal& m) {
        lv_obj_t* btn = lv_obj_find_by_name(m.dialog(), "btn_tertiary");
        if (!btn) return true;
        return lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }

    // Invokes the reset callback the same way the registered XML click handler would.
    static void press_reset(FilamentCatalogPickerModal& m) {
        if (m.reset_callback_) m.reset_callback_();
        m.hide();
    }
};

} // namespace helix::ui

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pre_print_options_renderer.h"

#include "ui_switch.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix::ui {

namespace {

/// Per-row event-callback context. Stored as the user_data of the switch
/// widget so the dispatcher can recover both the renderer and the option id
/// at event time. Owned by the row's `LV_EVENT_DELETE` callback (set up
/// alongside the value-changed callback) so it never leaks.
struct SwitchUserData {
    PrePrintOptionsRenderer* renderer;
    std::string id;
};

/// Translation tags follow the existing convention of using the English
/// label as both key and default value (see `translations/en.yml`). For
/// options without an explicit `label_key` in the database we fall back to a
/// best-effort humanization of the id ("ai_detect" -> "AI Detect"). New
/// options should declare `label_key` directly to bypass this path.
std::string humanize_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());

    bool capitalize_next = true;
    for (char c : id) {
        if (c == '_' || c == '-') {
            out.push_back(' ');
            capitalize_next = true;
            continue;
        }
        if (capitalize_next) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            capitalize_next = false;
        } else {
            out.push_back(c);
        }
    }

    // Common acronyms: don't lowercase the rest of these even when not first.
    // (We only need the ones that show up in our id space.)
    auto fix_acronym = [&](const std::string& word, const std::string& upper) {
        std::size_t pos = 0;
        while ((pos = out.find(word, pos)) != std::string::npos) {
            // Word-boundary check: previous char is start or space; next char is end or space.
            bool left_ok = (pos == 0) || (out[pos - 1] == ' ');
            bool right_ok = (pos + word.size() == out.size()) || (out[pos + word.size()] == ' ');
            if (left_ok && right_ok) {
                out.replace(pos, word.size(), upper);
                pos += upper.size();
            } else {
                pos += word.size();
            }
        }
    };
    fix_acronym("Ai", "AI");
    fix_acronym("Qgl", "QGL");
    fix_acronym("Pa", "PA");
    fix_acronym("Z tilt", "Z-tilt");

    return out;
}

} // namespace

PrePrintOptionsRenderer::~PrePrintOptionsRenderer() {
    // clear() walks each row and explicitly deinits its subject. Even when
    // the container has already been deleted (its observers gone), calling
    // lv_subject_deinit on a subject with no observers is a no-op cleanup
    // that frees its internal lists. Skipping this would leak the
    // subject->subs_ll backing on every detail-view destroy cycle.
    clear();
}

std::string PrePrintOptionsRenderer::label_for(const PrePrintOption& opt) {
    // First preference: explicit i18n key from the DB.
    if (!opt.label_key.empty()) {
        return std::string(lv_tr(opt.label_key.c_str()));
    }

    // Second preference: existing baked-in labels for the original four
    // mechanical/quality toggles. Phase 4 should replace these with proper
    // `label_key` entries in printer_database.json.
    if (opt.id == "bed_mesh") {
        // Single toggle relabels when adaptive meshing is available for this
        // printer (set by PrinterState::apply_dynamic_options). No separate row.
        return std::string(lv_tr(opt.adaptive_active ? "Adaptive Bed Mesh" : "Auto Bed Mesh"));
    }
    if (opt.id == "qgl") {
        return std::string(lv_tr("Quad Gantry Level"));
    }
    if (opt.id == "z_tilt") {
        return std::string(lv_tr("Z-Tilt Adjust"));
    }
    if (opt.id == "nozzle_clean") {
        return std::string(lv_tr("Clean Nozzle"));
    }
    if (opt.id == "purge_line") {
        return std::string(lv_tr("Nozzle Priming"));
    }
    if (opt.id == "timelapse") {
        return std::string(lv_tr("Record Timelapse"));
    }

    // Fallback: humanize the id. Run through lv_tr in case a translator added
    // an entry under the humanized form.
    std::string humanized = humanize_id(opt.id);
    return std::string(lv_tr(humanized.c_str()));
}

void PrePrintOptionsRenderer::populate(lv_obj_t* container, const PrePrintOptionSet& option_set,
                                       const VisibilitySubjectLookup& visibility_lookup,
                                       OnToggleCallback on_toggle) {
    if (!container) {
        spdlog::warn("[PrePrintOptionsRenderer] populate() called with null container");
        return;
    }

    on_toggle_ = std::move(on_toggle);

    // Order matters: deinit subjects (observers attached to live widgets) BEFORE
    // we ask LVGL to delete those widgets. If we deleted widgets first,
    // `safe_clean_children`'s widget cleanup would later fire each observer's
    // LV_EVENT_DELETE handler, and that handler calls `lv_observer_remove` on
    // the still-live subject — except in our case the subject is about to
    // disappear and our deinit walk would race the widget-side cleanup. By
    // deiniting subjects first, we walk each subject's observer list and
    // uninstall the per-observer state from the (still-alive) widget, so the
    // subsequent widget delete has nothing to do for those observers. The
    // alternative ordering would risk `lv_observer_remove` touching a freed
    // subject's `subs_ll` (UAF).
    clear();
    safe_clean_children(container);

    if (option_set.options.empty()) {
        spdlog::debug("[PrePrintOptionsRenderer] Empty option set — container left empty");
        return;
    }

    // Flat list of rows in (category, order) sort order. Categories serve as
    // a sort key only — no subheaders rendered. The single PRINT OPTIONS card
    // header in print_file_detail.xml acts as the section title.
    for (const auto& opt : option_set.options) {
        make_row(container, opt, visibility_lookup);
    }

    spdlog::debug("[PrePrintOptionsRenderer] Populated {} option row(s)", rows_.size());
}

void PrePrintOptionsRenderer::clear() {
    // Each OptionRow owns its lv_subject_t via unique_ptr; we must call
    // lv_subject_deinit explicitly before the unique_ptr drops the
    // allocation, otherwise the subject's observer linked-list backing
    // leaks. Observers themselves are owned by the row widgets that were
    // already deleted (or are being deleted by safe_clean_children); LVGL
    // auto-removes observers on widget delete, so by the time we get here
    // the subject's observer list is already empty.
    for (auto& row : rows_) {
        if (row.state_subject) {
            lv_subject_deinit(row.state_subject.get());
        }
    }
    rows_.clear();
    on_toggle_ = nullptr;
}

int PrePrintOptionsRenderer::get_state(const std::string& id, int default_if_missing) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return default_if_missing;
    }
    return lv_subject_get_int(it->state_subject.get());
}

void PrePrintOptionsRenderer::set_state(const std::string& id, int new_state) {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    if (it == rows_.end() || !it->state_subject) {
        return;
    }
    lv_subject_set_int(it->state_subject.get(), new_state);
}

std::vector<std::string> PrePrintOptionsRenderer::rendered_ids() const {
    std::vector<std::string> ids;
    ids.reserve(rows_.size());
    for (const auto& r : rows_) {
        ids.push_back(r.id);
    }
    return ids;
}

lv_obj_t* PrePrintOptionsRenderer::get_row(const std::string& id) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    return (it != rows_.end()) ? it->row : nullptr;
}

lv_obj_t* PrePrintOptionsRenderer::get_switch(const std::string& id) const {
    auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const OptionRow& r) { return r.id == id; });
    return (it != rows_.end()) ? it->switch_widget : nullptr;
}

void PrePrintOptionsRenderer::make_row(lv_obj_t* container, const PrePrintOption& opt,
                                       const VisibilitySubjectLookup& visibility_lookup) {
    OptionRow row;
    row.id = opt.id;

    // Heap-allocate the state subject so it survives container rebuilds where
    // OptionRow values may be moved around.
    row.state_subject = std::make_unique<lv_subject_t>();
    lv_subject_init_int(row.state_subject.get(), opt.default_enabled ? 1 : 0);

    // Row container: horizontal flex, label on left, switch on right.
    row.row = lv_obj_create(container);
    lv_obj_remove_style_all(row.row);
    lv_obj_set_width(row.row, lv_pct(100));
    lv_obj_set_height(row.row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row.row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row.row, LV_OBJ_FLAG_SCROLLABLE);

    // Label
    lv_obj_t* label = lv_label_create(row.row);
    std::string text = label_for(opt);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

    // Themed switch — same styling as <ui_switch size="small"/> from XML:
    // theme colors, size preset, value-changed sound callback all applied.
    lv_obj_t* sw = ui_switch_create_themed(row.row, "small");
    if (opt.default_enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    row.switch_widget = sw;

    // Bind switch checked-state to the per-option state subject. We do this
    // imperatively rather than via the XML <bind_state_if_eq> path because
    // the row is created dynamically.
    lv_subject_add_observer_obj(
        row.state_subject.get(),
        [](lv_observer_t* observer, lv_subject_t* subject) {
            auto* widget = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
            if (lv_subject_get_int(subject) == 1) {
                lv_obj_add_state(widget, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(widget, LV_STATE_CHECKED);
            }
        },
        sw, nullptr);

    // Visibility binding — `bind_flag_if_eq subject="..." flag="hidden" ref_value="0"`.
    if (visibility_lookup) {
        if (lv_subject_t* vis = visibility_lookup(opt.id)) {
            lv_subject_add_observer_obj(
                vis,
                [](lv_observer_t* observer, lv_subject_t* subject) {
                    auto* widget = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
                    if (lv_subject_get_int(subject) == 0) {
                        lv_obj_add_flag(widget, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_remove_flag(widget, LV_OBJ_FLAG_HIDDEN);
                    }
                },
                row.row, nullptr);
        }
    }

    // Wire up the value-changed callback. SwitchUserData is heap-allocated
    // and freed in the LV_EVENT_DELETE handler so it lives exactly as long
    // as the switch widget.
    auto* user_data = new SwitchUserData{this, opt.id};
    lv_obj_add_event_cb(sw, on_switch_value_changed, LV_EVENT_VALUE_CHANGED, user_data);
    lv_obj_add_event_cb(
        sw,
        [](lv_event_t* e) {
            // Free the heap-allocated user data when the widget is deleted.
            // LV_EVENT_DELETE callbacks fire exactly once.
            auto* data = static_cast<SwitchUserData*>(lv_event_get_user_data(e));
            delete data;
        },
        LV_EVENT_DELETE, user_data);

    rows_.push_back(std::move(row));
}

void PrePrintOptionsRenderer::on_switch_value_changed(lv_event_t* e) {
    // user_data lifetime is guaranteed by the LV_EVENT_DELETE handler, which
    // LVGL fires last among a widget's event callbacks. VALUE_CHANGED can
    // never fire after `data` is freed because deletion runs first only on
    // the LV_EVENT_DELETE path, which then frees `data` once.
    auto* data = static_cast<SwitchUserData*>(lv_event_get_user_data(e));
    if (!data || !data->renderer) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int new_state = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    data->renderer->set_state(data->id, new_state);
    if (data->renderer->on_toggle_) {
        data->renderer->on_toggle_(data->id, new_state);
    }
}

} // namespace helix::ui

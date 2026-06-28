// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_tool_text.h"

#include "ams_state.h"
#include "observer_factory.h"
#include "static_subject_registry.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

static ObserverGuard s_tool_text_observer;
static ObserverGuard s_toolchange_total_observer;
static ObserverGuard s_toolchange_current_observer;
static ObserverGuard s_tool_badge_observer;
static bool s_initialized = false;

static void update_toolchange_text(AmsState* a) {
    int total = lv_subject_get_int(a->get_ams_number_of_toolchanges_subject());
    if (total > 0) {
        int current = lv_subject_get_int(a->get_ams_current_toolchange_subject());
        int display_current = std::max(0, current + 1); // 1-based
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", display_current, total);
        lv_subject_copy_string(a->get_toolchange_text_subject(), buf);
    } else {
        lv_subject_copy_string(a->get_toolchange_text_subject(), "");
    }
}

namespace helix::ui {

void init_ams_tool_text_observers() {
    if (s_initialized) {
        return;
    }

    auto& ams = AmsState::instance();

    // Observer on raw ams_current_tool_ (int) → format "T%d" or "---"
    s_tool_text_observer =
        observe_int_sync<AmsState>(ams.get_current_tool_subject(), &ams, [](AmsState* a, int tool) {
            if (tool >= 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "T%d", tool);
                lv_subject_copy_string(a->get_current_tool_text_subject(), buf);
            } else {
                lv_subject_copy_string(a->get_current_tool_text_subject(), "---");
            }
        });

    // Two observers for toolchange text: one on total, one on current index
    s_toolchange_total_observer =
        observe_int_sync<AmsState>(ams.get_ams_number_of_toolchanges_subject(), &ams,
                                   [](AmsState* a, int /*total*/) { update_toolchange_text(a); });

    s_toolchange_current_observer =
        observe_int_sync<AmsState>(ams.get_ams_current_toolchange_subject(), &ams,
                                   [](AmsState* a, int /*current*/) { update_toolchange_text(a); });

    // Observer on tools_version_ → update tool badge text and visibility
    auto& tools = ToolState::instance();
    s_tool_badge_observer = observe_int_sync<ToolState>(
        tools.get_tools_version_subject(), &tools, [](ToolState* ts, int /*version*/) {
            const auto* tool = ts->is_multi_tool() ? ts->active_tool() : nullptr;
            if (tool) {
                lv_subject_copy_string(ts->get_tool_badge_text_subject(), tool->name.c_str());
                lv_subject_set_int(ts->get_show_tool_badge_subject(), 1);
            } else {
                lv_subject_copy_string(ts->get_tool_badge_text_subject(), "");
                lv_subject_set_int(ts->get_show_tool_badge_subject(), 0);
            }
        });

    s_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("AmsToolTextObservers", []() {
        if (s_initialized) {
            s_tool_text_observer.release();
            s_toolchange_total_observer.release();
            s_toolchange_current_observer.release();
            s_tool_badge_observer.release();
            s_initialized = false;
            spdlog::trace("[AmsToolText] Observers released");
        }
    });

    spdlog::debug("[AmsToolText] Tool text observers initialized");
}

} // namespace helix::ui

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "overlay_base.h"

#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

OverlayBase::~OverlayBase() {
    // Fallback unregister in case cleanup() wasn't called.
    // Guard against Static Destruction Order Fiasco: during shutdown,
    // NavigationManager may already be destroyed.
    if (overlay_root_ && !NavigationManager::is_destroyed()) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Guard against Static Destruction Order Fiasco: spdlog may already be
    // destroyed if this overlay wasn't registered with StaticPanelRegistry.
    if (!NavigationManager::is_destroyed()) {
        spdlog::trace("[OverlayBase] Destroyed");
    }
}

void OverlayBase::on_activate() {
    spdlog::trace("[OverlayBase] on_activate() - {}", get_name());
    visible_ = true;
}

void OverlayBase::on_deactivate() {
    spdlog::trace("[OverlayBase] on_deactivate() - {}", get_name());
    lifetime_.invalidate();
    visible_ = false;
}

void OverlayBase::cleanup() {
    spdlog::trace("[OverlayBase] cleanup() - {}", get_name());
    lifetime_.invalidate();
    cleanup_called_ = true;
    visible_ = false;
}

void OverlayBase::destroy_overlay_ui(lv_obj_t*& cached_panel) {
    if (!overlay_root_) {
        return;
    }

    spdlog::info("[{}] Destroying overlay UI to free memory", get_name());

    // Drain deferred observer callbacks while all pointers are still valid.
    // observe_int_sync queues lambdas via queue_update() that capture raw
    // panel pointers. Processing them here prevents use-after-free.
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // Unregister from NavigationManager before deleting the widget
    NavigationManager::instance().unregister_overlay_close_callback(overlay_root_);
    NavigationManager::instance().unregister_overlay_instance(overlay_root_);

    // Breadcrumb the destroy so crashes in the close path can be pinned to
    // which overlay was being torn down. Pairs with existing "overlay+" crumb
    // on push.
    crash_handler::breadcrumb::note("ovrl_dst", get_name());

    // Deferred delete: sync `safe_delete` is banned in overlay close callbacks
    // (ui_utils.h) — multiple sync deletions in the same UpdateQueue batch
    // corrupt LVGL's global event list (#776, #190, #80, #840).
    // destroy_overlay_ui runs as a close callback on memory-constrained devices
    // via register_overlay_close_callback(), so deferral is mandatory here.
    helix::ui::safe_delete_deferred(overlay_root_);

    // Also null the caller's cached pointer (may be the same as overlay_root_,
    // but could be a separate copy held by the calling panel)
    cached_panel = nullptr;

    // Let derived class null its widget pointers. With deferred deletion the
    // child widgets are still valid (hidden, reparented to top layer) during
    // this call; the async tick will recursively delete the whole subtree.
    on_ui_destroyed();
}

lv_obj_t* OverlayBase::create_overlay_from_xml(lv_obj_t* parent, const char* component_name) {
    if (!parent) {
        spdlog::error("[{}] Cannot create: null parent", get_name());
        return nullptr;
    }

    spdlog::debug("[{}] Creating overlay from XML", get_name());

    parent_screen_ = parent;
    cleanup_called_ = false;

    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, component_name, nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create from XML", get_name());
        return nullptr;
    }

    ui_overlay_panel_setup_standard(overlay_root_, parent_screen_, "overlay_header",
                                    "overlay_content");
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    return overlay_root_;
}

bool OverlayBase::rebuild() {
    if (!overlay_root_ || !parent_screen_) {
        spdlog::debug("[OverlayBase::rebuild] {} — no widget, skipping", get_name());
        return false;
    }

    spdlog::info("[OverlayBase::rebuild] {} — tearing down and re-creating", get_name());

    bool was_visible = visible_;

    on_deactivate(); // invalidates lifetime_, clears visible_

    lv_obj_t* old_root = overlay_root_;
    overlay_root_ = nullptr;

    lv_obj_t* new_root = create(parent_screen_);
    if (!new_root) {
        spdlog::error("[OverlayBase::rebuild] {} — create() returned null, restoring old widget",
                      get_name());
        overlay_root_ = old_root;
        if (was_visible) {
            visible_ = true;
            on_activate();
        }
        return false;
    }

    // create() sets overlay_root_ internally; some implementations also call
    // register_callbacks() — let them manage their own wiring.
    overlay_root_ = new_root;

    if (was_visible) {
        lv_obj_remove_flag(new_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(new_root, LV_OBJ_FLAG_HIDDEN);
    }

    NavigationManager::instance().rekey_overlay_widget(old_root, new_root);

    helix::ui::safe_delete_deferred(old_root);

    if (was_visible) {
        on_activate();
    }
    return true;
}

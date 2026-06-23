// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class SoundPreviewOverlay : public OverlayBase {
  public:
    SoundPreviewOverlay();
    ~SoundPreviewOverlay() override = default;

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    const char* get_name() const override {
        return "Preview Sounds";
    }

    void on_activate() override;
    void on_deactivate() override;

    void show(lv_obj_t* parent_screen);

  private:
    void populate_buttons();
    void clear_buttons();

    static std::string display_name(const std::string& sound_name);
};

SoundPreviewOverlay& get_sound_preview_overlay();

} // namespace helix::settings

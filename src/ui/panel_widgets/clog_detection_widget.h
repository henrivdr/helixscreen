// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <memory>
#include <string>

class ClogDetectionConfigModal;

namespace helix {
namespace ui {
class UiClogMeter;
class UiBufferMeter;
} // namespace ui

/// Panel widget for filament health monitoring on the home panel.
/// Shows a carousel with clog arc meter and optional buffer meter pages.
class ClogDetectionWidget : public PanelWidget {
  public:
    ClogDetectionWidget() = default;
    ~ClogDetectionWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    void on_activate() override;
    const char* id() const override {
        return "clog_detection";
    }

    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

  private:
    void apply_config();
    void build_carousel_pages();

    nlohmann::json config_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* carousel_ = nullptr;
    lv_obj_t* clog_page_ = nullptr;
    lv_obj_t* buffer_page_ = nullptr;
    std::unique_ptr<ui::UiClogMeter> clog_meter_;
    std::unique_ptr<ui::UiBufferMeter> buffer_meter_;
    std::unique_ptr<ClogDetectionConfigModal> config_modal_;
    bool has_buffer_page_ = false;
};

} // namespace helix

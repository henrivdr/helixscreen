// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"

#include <lvgl.h>
#include <vector>

/**
 * @brief Windows 95-style Starfield screensaver
 *
 * Stars fly outward from the center of the screen. Each star starts small
 * and dim near the center, growing larger and brighter as it approaches
 * the edges.
 *
 * Renders via direct pixel writes to the canvas buffer — no LVGL draw API
 * in the hot loop. Previous star positions are incrementally erased each
 * frame to avoid a full-buffer clear.
 */
class StarfieldScreensaver : public Screensaver {
  public:
    StarfieldScreensaver() = default;
    ~StarfieldScreensaver() override = default;

    void start() override;
    void stop() override;
    bool is_active() const override {
        return active_;
    }
    ScreensaverType type() const override {
        return ScreensaverType::STARFIELD;
    }

  private:
    struct Star {
        float x;        // normalized position (-1..1)
        float y;        // normalized position (-1..1)
        float z;        // depth (0..1, 1=far, approaches 0)
        float speed;    // z decrement per frame
        uint8_t tint_r; // color tint (assigned at birth, visible when close)
        uint8_t tint_g;
        uint8_t tint_b;
        // Previous frame screen position for incremental erase
        int16_t prev_sx;
        int16_t prev_sy;
        uint8_t prev_size; // 0 = not yet drawn
    };

    void init_stars();
    void recycle_star(Star& star);

    static void frame_timer_cb(lv_timer_t* timer);
    void render_frame();

    bool active_ = false;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    // Draw buffer owned by the canvas
    uint8_t* draw_buf_ = nullptr;

    std::vector<Star> stars_;

    // Cached screen dimensions and projection constants
    int screen_w_ = 0;
    int screen_h_ = 0;
    float cx_ = 0;    // screen center X
    float cy_ = 0;    // screen center Y
    float focal_ = 0; // projection focal length
};

#endif // HELIX_ENABLE_SCREENSAVER

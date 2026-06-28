// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "performance_source.h"

#include <atomic>
#include <random>

namespace helix {
namespace perf {

/// Mock source: real host /proc reads + synthetic MCU values.
/// Used in --test mode or when Moonraker is mocked.
class MockPerformanceSource : public IPerformanceSource {
  public:
    MockPerformanceSource();
    ~MockPerformanceSource() override;

    void start() override;
    void stop() override;
    void set_callback(SampleCallback cb) override {
        cb_ = std::move(cb);
    }

  private:
    void tick(); ///< called on the main thread, ~1 Hz via lv_timer

    bool read_host_stats(PerfSample& out);
    void synth_mcus(PerfSample& out);
    void apply_throttle_env(PerfSample& out);

    SampleCallback cb_;
    AsyncLifetimeGuard lifetime_;
    std::atomic<bool> running_{false};
    lv_timer_t* timer_ = nullptr;

    // /proc/stat delta state
    uint64_t prev_idle_ = 0;
    uint64_t prev_total_ = 0;
    bool have_prev_ = false;

    // Synthetic MCU walk state
    std::mt19937 rng_{0xC0FFEE};
    float mcu_load_ = 0.10f;
    float mcu_sb_load_ = 0.20f;
    int tick_count_ = 0;
};

} // namespace perf
} // namespace helix

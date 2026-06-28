// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_bitmap.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/// Label media size definition
struct LabelSize {
    std::string name; // Human-readable: "29mm", "62mm", "29x90mm"
    int width_px;     // Print width in pixels at native DPI
    int height_px;    // 0 = continuous (auto-size based on content)
    int dpi = 300;
    uint8_t media_type; // Protocol-specific media type byte
    uint8_t width_mm;   // Physical width in mm
    uint8_t length_mm;  // Physical length in mm (0 for continuous)
};

/// Label layout preset
enum class LabelPreset { STANDARD, COMPACT, MINIMAL };

/// Get human-readable name for a preset
const char* label_preset_name(LabelPreset preset);

/// Get all preset names as newline-separated string (for dropdown)
const char* label_preset_options();

/// Callback for async print completion
using PrintCallback = std::function<void(bool success, const std::string& error)>;

/**
 * @brief Abstract label printer interface
 *
 * Provides a common interface for different label printer backends
 * (network, USB, etc.). Implementations handle protocol-specific
 * communication.
 */
class ILabelPrinter {
  public:
    virtual ~ILabelPrinter() = default;

    /// Human-readable printer name (e.g. "Brother QL", "Phomemo M110")
    [[nodiscard]] virtual std::string name() const = 0;

    /// Print a label bitmap asynchronously. Callback fires on UI thread.
    virtual void print(const LabelBitmap& bitmap, const LabelSize& size,
                       PrintCallback callback) = 0;

    /// Get supported label sizes for this printer
    [[nodiscard]] virtual std::vector<LabelSize> supported_sizes() const = 0;
};

} // namespace helix

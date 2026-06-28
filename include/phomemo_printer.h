// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Phomemo M110 USB label printer backend
 *
 * Implements the Phomemo raster protocol over USB bulk transfer.
 * USB VID:PID 0x0493:0x8760, 203 DPI, 20-50mm print width.
 *
 * Thread safety: print() runs async on a detached thread. Callbacks
 * are dispatched to the UI thread via queue_update().
 */
class PhomemoPrinter : public ILabelPrinter {
  public:
    PhomemoPrinter();
    ~PhomemoPrinter() override;

    PhomemoPrinter(const PhomemoPrinter&) = delete;
    PhomemoPrinter& operator=(const PhomemoPrinter&) = delete;

    // === ILabelPrinter interface ===

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    // === Phomemo-specific API ===

    /// Set USB device identifiers for printing
    void set_device(uint16_t vid, uint16_t pid, const std::string& serial = "");

    /// Get supported label sizes for Phomemo M110 printers (static access)
    static std::vector<LabelSize> supported_sizes_static();

    /// Build the raw raster command buffer (public for testing)
    static std::vector<uint8_t> build_raster_commands(const LabelBitmap& bitmap,
                                                      const LabelSize& size);

  private:
    uint16_t vid_ = 0;
    uint16_t pid_ = 0;
    std::string serial_;
};

} // namespace helix

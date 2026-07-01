// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"
#include "sheet_label_layout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief IPP sheet label printer backend
 *
 * Prints filament spool labels on sheets of Avery-style labels using
 * standard IPP printers (inkjet/laser). Sends PWG Raster over HTTP POST.
 *
 * Thread safety: print() runs async on a detached thread. Callbacks
 * are dispatched to the UI thread via queue_update().
 */
class IppPrinter : public ILabelPrinter {
  public:
    IppPrinter();
    ~IppPrinter() override;

    // Non-copyable
    IppPrinter(const IppPrinter&) = delete;
    IppPrinter& operator=(const IppPrinter&) = delete;

    // === ILabelPrinter interface ===

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    // === IPP-specific API ===

    /// Set the target printer (host, port, resource path from mDNS)
    void set_target(const std::string& host, uint16_t port,
                    const std::string& resource_path = "ipp/print");

    /// Set the sheet template index (into get_sheet_templates())
    void set_sheet_template(int index);

    /// Set number of labels to print per sheet (0 = fill entire sheet)
    void set_label_count(int count);

    /// Set start position on sheet (0-based, skip already-used labels)
    void set_start_position(int start);

    /// Get supported sizes (returns LabelSize for each sheet template)
    static std::vector<LabelSize> supported_sizes_static();

  private:
    std::string host_;
    uint16_t port_ = 631;
    std::string resource_path_ = "ipp/print";
    int sheet_template_index_ = 0;
    int label_count_ = 1;
    int start_position_ = 0;
};

} // namespace helix

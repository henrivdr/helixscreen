// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <string>

namespace helix::label {

/// Brother QL label printer backend over Bluetooth RFCOMM (SPP).
/// Uses BluetoothLoader for RFCOMM connection and brother_ql_build_raster()
/// for protocol byte generation. Per-connection BT context (init on print,
/// deinit after). Async via detached thread + callback via queue_update().
class BrotherQLBluetoothPrinter : public ILabelPrinter {
  public:
    /// Set the Bluetooth device to print to
    void set_device(const std::string& mac, int channel = 1);

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

  private:
    std::string mac_;
    int channel_ = 1;
};

} // namespace helix::label

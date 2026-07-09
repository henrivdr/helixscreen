// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <string>

namespace helix::label {

/// Niimbot label printer backend over BLE GATT.
/// Supports B21, D11, D110 models using the Niimbot custom BLE protocol.
/// Uses BluetoothLoader for BLE connectivity and niimbot_build_print_job()
/// for protocol packet generation. Per-connection BT context (init on print,
/// deinit after). Async via detached thread + callback via queue_update().
class NiimbotBluetoothPrinter : public ILabelPrinter {
  public:
    /// Set the BLE device to print to (name used for model detection: B21 vs D11)
    void set_device(const std::string& mac, const std::string& device_name = {});

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

  private:
    std::string mac_;
    std::string name_;
};

} // namespace helix::label

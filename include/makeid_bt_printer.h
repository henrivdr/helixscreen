// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <string>

namespace helix::label {

/// MakeID (Wewin) label printer backend over BLE GATT.
/// Supports L1, M1, E1 and other MakeID models using the Wewin 0x66 protocol.
/// Uses BluetoothLoader for BLE connectivity and makeid_build_print_job()
/// for protocol frame generation. Persistent BLE connection across prints.
/// Async via detached thread + callback via queue_update().
class MakeIdBluetoothPrinter : public ILabelPrinter {
  public:
    /// Set the BLE device to print to (name used for model detection)
    void set_device(const std::string& mac, const std::string& device_name = {});

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

  private:
    std::string mac_;
    std::string name_;
};

} // namespace helix::label

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct _lv_timer_t;

namespace helix {

/// Information about a detected USB printer
struct UsbPrinterInfo {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string serial;
    std::string product_name;
    uint8_t bus = 0;
    uint8_t address = 0;
};

/// Entry in the known USB printer table
struct KnownUsbPrinter {
    uint16_t vid;
    uint16_t pid;
    std::string name;
};

/**
 * @brief Detects known USB label printers via libusb
 *
 * Scans the USB bus for devices matching known VID:PID pairs.
 * Supports one-shot scanning and periodic polling with change detection.
 *
 * Polling uses an LVGL timer, so start/stop must be called from the
 * LVGL thread. The detection callback runs directly on the UI thread.
 */
class UsbPrinterDetector {
  public:
    UsbPrinterDetector();
    ~UsbPrinterDetector();

    // Non-copyable
    UsbPrinterDetector(const UsbPrinterDetector&) = delete;
    UsbPrinterDetector& operator=(const UsbPrinterDetector&) = delete;

    using DetectionCallback = std::function<void(const std::vector<UsbPrinterInfo>&)>;

    /// Scan once for known USB printers (synchronous)
    std::vector<UsbPrinterInfo> scan();

    /// Start periodic scanning. Callback fires directly on UI thread.
    void start_polling(DetectionCallback callback, int interval_ms = 3000);

    /// Stop periodic scanning
    void stop_polling();

    /// Whether periodic polling is active
    [[nodiscard]] bool is_polling() const;

    /// Known printer VID:PID table
    static const std::vector<KnownUsbPrinter>& known_printers();

    /// Check if a VID:PID is in the known table
    static bool is_known_printer(uint16_t vid, uint16_t pid);

    /// Get name for a known VID:PID (empty if unknown)
    static std::string get_printer_name(uint16_t vid, uint16_t pid);

  private:
    static void poll_timer_cb(_lv_timer_t* timer);

    /// Compare two result sets by VID+PID+bus+address
    static bool results_equal(std::vector<UsbPrinterInfo> a, std::vector<UsbPrinterInfo> b);

    _lv_timer_t* poll_timer_ = nullptr;
    DetectionCallback callback_;
    std::vector<UsbPrinterInfo> last_detected_;
    bool first_scan_ = true;
};

} // namespace helix

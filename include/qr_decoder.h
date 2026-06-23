// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

struct quirc;

namespace helix {

struct QrDecodeResult {
    bool success = false;
    std::string text;
    int spool_id = -1; // Parsed Spoolman spool ID, or -1
};

class QrDecoder {
  public:
    QrDecoder();
    ~QrDecoder();

    // Non-copyable (quirc context)
    QrDecoder(const QrDecoder&) = delete;
    QrDecoder& operator=(const QrDecoder&) = delete;

    // Decode QR from grayscale image buffer.
    // NOT thread-safe — each thread must use its own QrDecoder instance.
    QrDecodeResult decode(const uint8_t* gray_data, int width, int height);

    // Parse Spoolman spool ID from QR text string.
    // Handles: "web+spoolman:s-<id>", "SM:SPOOL=<id>", URL like "https://host/view/spool/<id>"
    // Returns -1 if not a recognized Spoolman format.
    static int parse_spoolman_id(const std::string& text);

  private:
    ::quirc* qr_ = nullptr;
};

} // namespace helix

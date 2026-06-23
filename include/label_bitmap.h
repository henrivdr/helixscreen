// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief 1-bit-per-pixel bitmap for label printing
 *
 * Stores a monochrome image packed 8 pixels per byte, MSB first.
 * Row data is byte-aligned (rows padded to whole bytes).
 * Black = 1, White = 0 (matching Brother QL raster format).
 */
class LabelBitmap {
  public:
    LabelBitmap() = default;

    LabelBitmap(int width, int height)
        : width_(width), height_(height), row_bytes_((width + 7) / 8),
          data_(static_cast<size_t>(row_bytes_) * height, 0x00) {}

    int width() const {
        return width_;
    }
    int height() const {
        return height_;
    }
    int row_byte_width() const {
        return row_bytes_;
    }

    const uint8_t* row_data(int y) const {
        return data_.data() + static_cast<size_t>(y) * row_bytes_;
    }

    uint8_t* row_data(int y) {
        return data_.data() + static_cast<size_t>(y) * row_bytes_;
    }

    const uint8_t* data() const {
        return data_.data();
    }
    size_t data_size() const {
        return data_.size();
    }

    /// Set a single pixel (1 = black, 0 = white)
    void set_pixel(int x, int y, bool black) {
        if (x < 0 || x >= width_ || y < 0 || y >= height_)
            return;
        int byte_idx = y * row_bytes_ + x / 8;
        uint8_t mask = 0x80 >> (x % 8);
        if (black)
            data_[byte_idx] |= mask;
        else
            data_[byte_idx] &= ~mask;
    }

    bool get_pixel(int x, int y) const {
        if (x < 0 || x >= width_ || y < 0 || y >= height_)
            return false;
        int byte_idx = y * row_bytes_ + x / 8;
        uint8_t mask = 0x80 >> (x % 8);
        return (data_[byte_idx] & mask) != 0;
    }

    /// Fill entire bitmap with value (true=black, false=white)
    void fill(bool black) {
        std::memset(data_.data(), black ? 0xFF : 0x00, data_.size());
    }

    bool empty() const {
        return data_.empty();
    }

    /// Composite src bitmap onto this bitmap at (dst_x, dst_y), clipping at boundaries
    void blit(const LabelBitmap& src, int dst_x, int dst_y);

    /// Rotate 90° clockwise: (x,y) → (height-1-y, x). Returns new bitmap.
    [[nodiscard]] LabelBitmap rotate_90_cw() const;

    /// Factory method (dpi parameter stored for metadata but doesn't affect pixel storage)
    static LabelBitmap create(int width, int height, int /*dpi*/ = 300) {
        return LabelBitmap(width, height);
    }

  private:
    int width_ = 0;
    int height_ = 0;
    int row_bytes_ = 0;
    std::vector<uint8_t> data_;
};

/// Generate a QR code bitmap at target pixel size
/// Uses qrcodegen with EC level HIGH (30% correction, supports logo overlay)
/// Returns empty bitmap on failure
LabelBitmap generate_qr_bitmap(const std::string& data, int target_size_px);

} // namespace helix

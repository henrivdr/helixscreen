// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "niimbot_protocol.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix::label {

std::vector<uint8_t> niimbot_build_packet(NiimbotCmd cmd, const uint8_t* data, size_t len) {
    // Niimbot protocol length field is a single byte
    if (len > 255)
        return {};

    // Packet: [0x55 0x55 CMD LEN DATA... CHECKSUM 0xAA 0xAA]
    std::vector<uint8_t> pkt;
    pkt.reserve(6 + len);

    pkt.push_back(0x55);
    pkt.push_back(0x55);
    pkt.push_back(static_cast<uint8_t>(cmd));
    pkt.push_back(static_cast<uint8_t>(len));

    // Checksum = XOR of cmd, length, and all data bytes
    uint8_t checksum = static_cast<uint8_t>(cmd) ^ static_cast<uint8_t>(len);
    for (size_t i = 0; i < len; i++) {
        pkt.push_back(data[i]);
        checksum ^= data[i];
    }

    pkt.push_back(checksum);
    pkt.push_back(0xAA);
    pkt.push_back(0xAA);

    return pkt;
}

/// Count set bits in a byte range
static int count_black_pixels(const uint8_t* data, size_t len) {
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        // Brian Kernighan's bit counting
        uint8_t b = data[i];
        while (b) {
            count++;
            b &= (b - 1);
        }
    }
    return count;
}

/// Build a PrintBitmapRow (0x85) packet for one image row.
/// Format: [ROW_H ROW_L COUNT1 COUNT2 COUNT3 REPEAT PIXEL_DATA...]
static std::vector<uint8_t> build_bitmap_row(int row_num, const uint8_t* pixels, int bytes_per_row,
                                             int printhead_pixels, int repeat_count = 1) {
    int chunk_size = printhead_pixels / 8 / 3;
    bool use_split = (bytes_per_row <= chunk_size * 3);

    std::vector<uint8_t> payload;
    payload.reserve(6 + bytes_per_row);

    // Row number (big-endian u16)
    payload.push_back(static_cast<uint8_t>((row_num >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(row_num & 0xFF));

    if (use_split) {
        // Split mode: count black pixels in each third
        for (int i = 0; i < 3; i++) {
            int offset = i * chunk_size;
            int len = std::min(chunk_size, bytes_per_row - offset);
            if (len > 0) {
                payload.push_back(static_cast<uint8_t>(count_black_pixels(pixels + offset, len)));
            } else {
                payload.push_back(0);
            }
        }
    } else {
        // Total mode: [0, LOW, HIGH] of total black count
        int total = count_black_pixels(pixels, bytes_per_row);
        payload.push_back(0);
        payload.push_back(static_cast<uint8_t>(total & 0xFF));
        payload.push_back(static_cast<uint8_t>((total >> 8) & 0xFF));
    }

    // Repeat count
    payload.push_back(static_cast<uint8_t>(repeat_count));

    // Pixel data
    payload.insert(payload.end(), pixels, pixels + bytes_per_row);

    return payload;
}

NiimbotPrintJob niimbot_build_print_job(const LabelBitmap& bitmap, const LabelSize& size,
                                        uint8_t density, NiimbotLabelType label_type) {
    NiimbotPrintJob job;
    density = std::clamp(density, uint8_t(1), uint8_t(5));

    int width_px = bitmap.width();
    int height_px = bitmap.height();
    int bytes_per_row = bitmap.row_byte_width();

    // Printhead width from label pixel width (B21=384px, D11=96px)
    int printhead_pixels = size.width_px;

    // Pad row to printhead width if needed
    int printhead_bytes = printhead_pixels / 8;
    if (bytes_per_row < printhead_bytes) {
        bytes_per_row = printhead_bytes;
    }

    // 1. SetDensity
    job.packets.push_back(niimbot_build_packet(NiimbotCmd::SetDensity, density));

    // 2. SetLabelType
    job.packets.push_back(
        niimbot_build_packet(NiimbotCmd::SetLabelType, static_cast<uint8_t>(label_type)));

    // 3. PrintStart
    job.packets.push_back(niimbot_build_packet(NiimbotCmd::PrintStart, uint8_t(0x01)));

    // 4. PrintClear (required by D110 before PageStart)
    job.packets.push_back(niimbot_build_packet(NiimbotCmd::PrintClear, uint8_t(0x01)));

    // 5. PageStart (payload 0x01 for D110)
    job.packets.push_back(niimbot_build_packet(NiimbotCmd::PageStart, uint8_t(0x01)));

    // 6. SetPageSize (4-byte: height_u16be, width_u16be)
    {
        uint8_t page_size[4];
        page_size[0] = static_cast<uint8_t>((height_px >> 8) & 0xFF);
        page_size[1] = static_cast<uint8_t>(height_px & 0xFF);
        page_size[2] = static_cast<uint8_t>((width_px >> 8) & 0xFF);
        page_size[3] = static_cast<uint8_t>(width_px & 0xFF);
        job.packets.push_back(niimbot_build_packet(NiimbotCmd::SetPageSize, page_size, 4));
    }

    // 7. SetPrintQuantity (1 copy — D110 requires this to avoid double-printing)
    {
        uint8_t qty[2] = {0x00, 0x01};
        job.packets.push_back(niimbot_build_packet(NiimbotCmd::SetQuantity, qty, 2));
    }

    spdlog::info("Niimbot job: bitmap {}x{}, bytes_per_row={}, printhead_bytes={}, density={}",
                 width_px, height_px, bytes_per_row, printhead_bytes, density);

    // 8. Image rows — pad each row to printhead width if needed
    int bmp_row_bytes = bitmap.row_byte_width();
    std::vector<uint8_t> padded_row(bytes_per_row, 0x00);

    auto get_row = [&](int y) -> const uint8_t* {
        if (bytes_per_row == bmp_row_bytes) {
            return bitmap.row_data(y);
        }
        // Pad: copy bitmap data then zero-fill remainder
        std::memcpy(padded_row.data(), bitmap.row_data(y), bmp_row_bytes);
        std::memset(padded_row.data() + bmp_row_bytes, 0, bytes_per_row - bmp_row_bytes);
        return padded_row.data();
    };

    int row = 0;
    while (row < height_px) {
        const uint8_t* row_ptr = get_row(row);

        // Check if row is all-white
        bool is_blank = true;
        for (int i = 0; i < bytes_per_row; i++) {
            if (row_ptr[i] != 0) {
                is_blank = false;
                break;
            }
        }

        if (is_blank) {
            // Count consecutive blank rows
            int blank_count = 1;
            while (row + blank_count < height_px && blank_count < 255) {
                const uint8_t* next = get_row(row + blank_count);
                bool next_blank = true;
                for (int i = 0; i < bytes_per_row; i++) {
                    if (next[i] != 0) {
                        next_blank = false;
                        break;
                    }
                }
                if (!next_blank)
                    break;
                blank_count++;
            }

            // PrintEmptyRow: [row_hi, row_lo, repeat]
            uint8_t empty_data[3] = {static_cast<uint8_t>((row >> 8) & 0xFF),
                                     static_cast<uint8_t>(row & 0xFF),
                                     static_cast<uint8_t>(blank_count)};
            job.packets.push_back(niimbot_build_packet(NiimbotCmd::PrintEmptyRow, empty_data, 3));
            row += blank_count;
        } else {
            // Copy current row for stable comparison (get_row reuses padded_row buffer)
            std::vector<uint8_t> current_row(row_ptr, row_ptr + bytes_per_row);

            // Check for repeated rows
            int repeat = 1;
            while (row + repeat < height_px && repeat < 255) {
                const uint8_t* next = get_row(row + repeat);
                if (memcmp(current_row.data(), next, bytes_per_row) != 0)
                    break;
                repeat++;
            }

            auto payload =
                build_bitmap_row(row, current_row.data(), bytes_per_row, printhead_pixels, repeat);
            job.packets.push_back(niimbot_build_packet(NiimbotCmd::PrintBitmapRow, payload));
            row += repeat;
        }
    }

    job.total_rows = height_px;

    // PageEnd (payload 0x01 for D110)
    job.packets.push_back(niimbot_build_packet(NiimbotCmd::PageEnd, uint8_t(0x01)));

    // NOTE: PrintEnd is NOT included in the packet list.
    // It must be sent AFTER polling PrintStatus confirms the page is complete.
    // See niimbot_bt_printer.cpp for the post-loop PrintEnd handling.

    return job;
}

std::vector<LabelSize> niimbot_b21_sizes() {
    // B21/B1/B3S/B18: shared 384px (48mm) wide printhead, 203 DPI.
    // ~7.68 dots/mm convention matches existing 50x30 → 384x231 entry.
    return {
        {"50x30mm", 384, 231, 203, 0x01, 50, 30}, {"40x30mm", 307, 231, 203, 0x01, 40, 30},
        {"30x20mm", 231, 154, 203, 0x01, 30, 20}, {"30x40mm", 231, 307, 203, 0x01, 30, 40},
        {"40x40mm", 307, 307, 203, 0x01, 40, 40}, {"50x50mm", 384, 384, 203, 0x01, 50, 50},
        {"40x20mm", 307, 154, 203, 0x01, 40, 20}, {"25x30mm", 192, 231, 203, 0x01, 25, 30},
        {"14x40mm", 108, 307, 203, 0x01, 14, 40}, {"14x60mm", 108, 461, 203, 0x01, 14, 60},
        {"50x80mm", 384, 615, 203, 0x01, 50, 80},
    };
}

std::vector<LabelSize> niimbot_d11_sizes() {
    // D11/D110: 96px (12mm) wide printhead, 203 DPI
    // All labels are 12mm wide; common lengths vary by label stock
    // 12x40mm is the default stock that ships with the printer
    return {
        {"12x40mm", 96, 307, 203, 0x01, 12, 40}, {"12x22mm", 96, 169, 203, 0x01, 12, 22},
        {"12x30mm", 96, 231, 203, 0x01, 12, 30}, {"12x50mm", 96, 384, 203, 0x01, 12, 50},
        {"12x60mm", 96, 461, 203, 0x01, 12, 60}, {"12x70mm", 96, 538, 203, 0x01, 12, 70},
    };
}

std::vector<LabelSize> niimbot_sizes_for_model(const std::string& device_name) {
    // D11/D110 have a 96px (12mm) printhead, everything else (B21 etc.) has 384px
    if (device_name.find("D11") != std::string::npos ||
        device_name.find("d11") != std::string::npos) {
        return niimbot_d11_sizes();
    }
    return niimbot_b21_sizes();
}

} // namespace helix::label

#endif // HELIX_HAS_LABEL_PRINTER

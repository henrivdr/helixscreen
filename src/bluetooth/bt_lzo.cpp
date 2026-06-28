// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth_plugin.h"

#include <cstring>
#include <minilzo.h>

static bool s_lzo_initialized = false;

extern "C" int helix_bt_lzo_compress(const uint8_t* in, int in_len, uint8_t* out, int out_len) {
    if (!in || in_len <= 0 || !out || out_len <= 0)
        return -1;

    if (!s_lzo_initialized) {
        if (lzo_init() != LZO_E_OK)
            return -2;
        s_lzo_initialized = true;
    }

    // LZO1X-1 requires a work memory buffer
    lzo_align_t work_mem[LZO1X_1_MEM_COMPRESS / sizeof(lzo_align_t) + 1];
    std::memset(work_mem, 0, sizeof(work_mem));

    lzo_uint dst_len = static_cast<lzo_uint>(out_len);
    int r =
        lzo1x_1_compress(reinterpret_cast<const unsigned char*>(in), static_cast<lzo_uint>(in_len),
                         reinterpret_cast<unsigned char*>(out), &dst_len, work_mem);

    if (r != LZO_E_OK)
        return -3;
    return static_cast<int>(dst_len);
}

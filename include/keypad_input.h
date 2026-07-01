// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>

namespace helix::ui {

/// Pure input buffer logic for the numeric keypad (no LVGL dependency).
struct KeypadInput {
    static constexpr size_t BUF_SIZE = 16;

    char buf[BUF_SIZE] = "";

    void clear() {
        buf[0] = '\0';
    }

    size_t length() const {
        return strlen(buf);
    }

    bool has_dot() const {
        return strchr(buf, '.') != nullptr;
    }

    /// Append a digit 0-9. Limits: 3 digits without decimal, 5 with.
    bool append_digit(int digit) {
        if (digit < 0 || digit > 9)
            return false;

        size_t len = length();
        int digit_count = 0;
        bool dot = false;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9')
                digit_count++;
            else if (buf[i] == '.')
                dot = true;
        }
        int max_digits = dot ? 5 : 3;
        if (digit_count >= max_digits)
            return false;

        if (len < BUF_SIZE - 1) {
            buf[len] = '0' + digit;
            buf[len + 1] = '\0';
            return true;
        }
        return false;
    }

    /// Append a decimal point. Only one allowed.
    bool append_dot() {
        if (has_dot())
            return false;
        size_t len = length();
        if (len < BUF_SIZE - 1) {
            buf[len] = '.';
            buf[len + 1] = '\0';
            return true;
        }
        return false;
    }

    /// Remove the last character.
    bool backspace() {
        size_t len = length();
        if (len == 0)
            return false;
        buf[len - 1] = '\0';
        return true;
    }

    /// Parse buffer as float. Empty buffer returns 0.
    float value() const {
        if (buf[0] == '\0')
            return 0.0f;
        return static_cast<float>(atof(buf));
    }
};

} // namespace helix::ui

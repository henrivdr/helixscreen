// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_bitmap.h"
#include "label_printer.h"

#include <string>

struct SpoolInfo;

namespace helix {

class LabelRenderer {
  public:
    /// Render a complete label for the given spool
    static LabelBitmap render(const SpoolInfo& spool, LabelPreset preset, const LabelSize& size);
};

} // namespace helix

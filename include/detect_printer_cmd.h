// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "printer_detector.h"

#include <string>
namespace helix::detect {
std::string format_detect_verdict(const PrinterDetectionResult& result,
                                  const std::string& runner_up_preset);
int run_detect_printer(const std::string& host, int port);
} // namespace helix::detect

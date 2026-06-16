// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include "hv/json.hpp"
#include "filament_mapper.h"
namespace helix {
std::vector<AvailableSlot> parse_snapmaker_task_config(const nlohmann::json& task_config);
}

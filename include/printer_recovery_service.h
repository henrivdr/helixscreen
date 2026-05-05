// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_error.h"

#include <functional>

class MoonrakerAPI;

namespace helix {

/// Platform-blind printer recovery orchestrator.
///
/// "Recovery" means: bring a Klipper instance back to a usable state when
/// `firmware_restart` alone won't do it (e.g. K2's `klipper_mcu` daemon needs
/// to be bounced because a CFS retract error left the rpi MCU shut down —
/// see `key298: Can not update MCU rpi config as it is shutdown`).
///
/// **Frontend stays platform-blind by convention:** every platform that
/// needs deeper recovery than `firmware_restart` ships a Moonraker
/// `[shell_command helix_recover]` block. We try it; if the command isn't
/// defined we fall through to `firmware_restart`. The per-platform installer
/// is responsible for writing that shell_command (and any companion gcode_macro)
/// — this class never learns platform names.
///
/// Lifetime: stateless free-standing helper, owned wherever it's invoked from
/// (typically the EmergencyStopOverlay or a toast action callback).
class PrinterRecoveryService {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;

    explicit PrinterRecoveryService(MoonrakerAPI* api) : api_(api) {}

    /// Run platform-correct deep recovery.
    ///
    /// Tries Moonraker `[shell_command helix_recover]` first. If Moonraker
    /// reports the command is undefined, falls back to `printer.firmware_restart`.
    /// Either way the success callback fires when the chosen path acks.
    void recover(SuccessCallback on_success, ErrorCallback on_error);

  private:
    MoonrakerAPI* api_;
};

} // namespace helix

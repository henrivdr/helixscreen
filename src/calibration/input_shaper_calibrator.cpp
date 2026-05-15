// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file input_shaper_calibrator.cpp
 * @brief Implementation of InputShaperCalibrator class
 *
 * Orchestrates input shaper calibration workflow using MoonrakerAPI.
 * Manages state transitions, result storage, and error handling.
 */

#include "input_shaper_calibrator.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"

#include <cctype>

namespace helix {
namespace calibration {

// ============================================================================
// Constructors
// ============================================================================

InputShaperCalibrator::InputShaperCalibrator() : api_(nullptr) {
    spdlog::debug("[InputShaperCalibrator] Created without API (test mode)");
}

InputShaperCalibrator::InputShaperCalibrator(MoonrakerAPI* api) : api_(api) {
    spdlog::debug("[InputShaperCalibrator] Created with API");
}

// ============================================================================
// ensure_homed_then()
// ============================================================================

void InputShaperCalibrator::ensure_homed_then(std::function<void()> then, ErrorCallback on_error) {
    // Check current homing state from PrinterState
    const char* homed = lv_subject_get_string(get_printer_state().get_homed_axes_subject());
    bool all_homed = homed && std::string(homed).find("xyz") != std::string::npos;

    if (all_homed) {
        spdlog::debug("[InputShaperCalibrator] Already homed, proceeding");
        then();
        return;
    }

    spdlog::info("[InputShaperCalibrator] Not fully homed (axes={}), sending G28",
                 homed ? homed : "none");

    api_->execute_gcode(
        "G28",
        [then = std::move(then)]() {
            spdlog::info("[InputShaperCalibrator] G28 complete, proceeding");
            then();
        },
        [this, on_error](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn(
                    "[InputShaperCalibrator] G28 response timed out (may still be running)");
                if (on_error) {
                    on_error("Homing timed out — printer may still be homing");
                }
            } else {
                spdlog::error("[InputShaperCalibrator] Homing failed: {}", err.message);
                if (on_error) {
                    on_error("Homing failed: " + err.message);
                }
            }
            state_ = State::IDLE;
        },
        MoonrakerAPI::HOMING_TIMEOUT_MS);
}

// ============================================================================
// check_accelerometer()
// ============================================================================

void InputShaperCalibrator::check_accelerometer(AccelCheckCallback on_complete,
                                                ErrorCallback on_error) {
    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] check_accelerometer called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Transition to CHECKING_ADXL state
    state_ = State::CHECKING_ADXL;
    spdlog::info("[InputShaperCalibrator] Starting accelerometer check");

    // Ensure homed before measuring (toolhead needs to be positioned)
    ensure_homed_then(
        [this, on_complete, on_error]() {
            api_->advanced().measure_axes_noise(
                [this, on_complete](float noise_level) {
                    results_.noise_level = noise_level;
                    state_ = State::IDLE;

                    spdlog::info(
                        "[InputShaperCalibrator] Accelerometer check complete, noise={:.4f}",
                        noise_level);

                    if (on_complete) {
                        on_complete(noise_level);
                    }
                },
                [this, on_error](const MoonrakerError& err) {
                    state_ = State::IDLE;

                    spdlog::error("[InputShaperCalibrator] Accelerometer check failed: {}",
                                  err.message);

                    if (on_error) {
                        on_error(err.message);
                    }
                });
        },
        on_error);
}

// ============================================================================
// run_calibration()
// ============================================================================

void InputShaperCalibrator::run_calibration(char axis, ProgressCallback on_progress,
                                            ResultCallback on_complete, ErrorCallback on_error) {
    // Normalize axis to uppercase
    char normalized_axis = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));

    // Validate axis
    if (normalized_axis != 'X' && normalized_axis != 'Y') {
        spdlog::warn("[InputShaperCalibrator] Invalid axis: {}", axis);
        if (on_error) {
            on_error("Invalid axis: " + std::string(1, axis) + " (must be X or Y)");
        }
        state_ = State::IDLE;
        return;
    }

    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] run_calibration called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    // Guard against concurrent runs - only allow from IDLE or READY states
    if (state_ != State::IDLE && state_ != State::READY) {
        spdlog::warn("[InputShaperCalibrator] Calibration already in progress (state={})",
                     static_cast<int>(state_));
        if (on_error) {
            on_error("Calibration already in progress");
        }
        return;
    }

    // Transition to appropriate testing state
    state_ = (normalized_axis == 'X') ? State::TESTING_X : State::TESTING_Y;
    spdlog::info("[InputShaperCalibrator] Starting calibration for axis {}", normalized_axis);

    // Ensure homed before running resonance test (needs absolute coordinates)
    ensure_homed_then(
        [this, normalized_axis, on_progress, on_complete, on_error]() {
            auto api_progress = [on_progress](int percent) {
                if (on_progress) {
                    on_progress(percent);
                }
            };

            api_->advanced().start_resonance_test(
                normalized_axis, api_progress,
                [this, normalized_axis, on_complete](const InputShaperResult& result) {
                    if (normalized_axis == 'X') {
                        results_.x_result = result;
                    } else {
                        results_.y_result = result;
                    }

                    if (results_.is_complete()) {
                        state_ = State::READY;
                        spdlog::info("[InputShaperCalibrator] Both axes calibrated, state=READY");
                    } else {
                        state_ = State::IDLE;
                        spdlog::info(
                            "[InputShaperCalibrator] Axis {} complete, awaiting other axis",
                            normalized_axis);
                    }

                    if (on_complete) {
                        on_complete(result);
                    }
                },
                [this, on_error](const MoonrakerError& err) {
                    state_ = State::IDLE;
                    spdlog::error("[InputShaperCalibrator] Calibration failed: {}", err.message);
                    if (on_error) {
                        on_error(err.message);
                    }
                });
        },
        on_error);
}

// ============================================================================
// emergency_abort()
// ============================================================================

void InputShaperCalibrator::emergency_abort() {
    state_ = State::IDLE;

    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] emergency_abort called without API");
        return;
    }

    spdlog::info("[InputShaperCalibrator] Emergency abort: sending M112 + firmware_restart");

    MoonrakerAPI* api = api_;
    api_->emergency_stop(
        [api]() {
            spdlog::debug("[InputShaperCalibrator] M112 sent, restarting firmware");
            api->restart_firmware(
                []() { spdlog::debug("[InputShaperCalibrator] Firmware restart initiated"); },
                [](const MoonrakerError& err) {
                    spdlog::error("[InputShaperCalibrator] Firmware restart failed: {}",
                                  err.message);
                });
        },
        [](const MoonrakerError& err) {
            spdlog::error("[InputShaperCalibrator] Emergency stop failed: {}", err.message);
        });
}

// ============================================================================
// apply_settings()
// ============================================================================

void InputShaperCalibrator::apply_settings(const ApplyConfig& config, SuccessCallback on_success,
                                           ErrorCallback on_error) {
    // Validate config - shaper_type must not be empty
    if (config.shaper_type.empty()) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called with empty shaper_type");
        if (on_error) {
            on_error("Invalid configuration: shaper_type cannot be empty");
        }
        return;
    }

    // Validate config - frequency must be positive
    if (config.frequency <= 0.0f) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called with invalid frequency: {}",
                     config.frequency);
        if (on_error) {
            on_error("Invalid configuration: frequency must be positive");
        }
        return;
    }

    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] apply_settings called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    spdlog::info("[InputShaperCalibrator] Applying settings: axis={}, type={}, freq={:.1f}Hz",
                 config.axis, config.shaper_type, config.frequency);

    // Adapt error callback from MoonrakerError to string
    auto error_adapter = [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    };

    // Call API to set input shaper
    api_->advanced().set_input_shaper(config.axis, config.shaper_type,
                                      static_cast<double>(config.frequency), on_success,
                                      error_adapter);
}

// ============================================================================
// save_to_config()
// ============================================================================

void InputShaperCalibrator::save_to_config(SuccessCallback on_success, ErrorCallback on_error) {
    // Check if API is available
    if (!api_) {
        spdlog::warn("[InputShaperCalibrator] save_to_config called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    spdlog::info("[InputShaperCalibrator] Saving configuration to printer.cfg");

    // Adapt error callback from MoonrakerError to string
    auto error_adapter = [on_error](const MoonrakerError& err) {
        if (on_error) {
            on_error(err.message);
        }
    };

    // Call API to save config
    api_->advanced().save_config(on_success, error_adapter);
}

} // namespace calibration
} // namespace helix

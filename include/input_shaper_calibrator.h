// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file input_shaper_calibrator.h
 * @brief High-level orchestrator for input shaper calibration workflow
 *
 * InputShaperCalibrator manages the complete calibration process:
 * 1. Check accelerometer connectivity and noise level
 * 2. Run resonance tests on X and Y axes
 * 3. Store and compare results
 * 4. Apply chosen settings to printer
 * 5. Save configuration to printer.cfg
 *
 * This is a state machine that coordinates MoonrakerAPI calls and
 * provides progress/error callbacks to the UI layer.
 */

#include "calibration_types.h"

#include <functional>
#include <string>

// Forward declaration
class MoonrakerAPI;

namespace helix {
namespace calibration {

/**
 * @brief Configuration for applying input shaper settings
 */
struct ApplyConfig {
    char axis = 'X';            ///< Axis to configure ('X' or 'Y')
    std::string shaper_type;    ///< Shaper type (e.g., "mzv", "ei")
    float frequency = 0.0f;     ///< Shaper frequency in Hz
    float damping_ratio = 0.1f; ///< Damping ratio (default 0.1)
};

/**
 * @brief Callback types for InputShaperCalibrator
 */
using AccelCheckCallback = std::function<void(float noise_level)>;
using ProgressCallback = std::function<void(int percent)>;
using ResultCallback = std::function<void(const InputShaperResult& result)>;
using SuccessCallback = std::function<void()>;
using ErrorCallback = std::function<void(const std::string& message)>;

/**
 * @brief High-level orchestrator for input shaper calibration workflow
 *
 * Manages the complete calibration process as a state machine:
 * - IDLE: Ready to start calibration
 * - CHECKING_ADXL: Verifying accelerometer connection
 * - TESTING_X: Running resonance test on X axis
 * - TESTING_Y: Running resonance test on Y axis
 * - READY: Calibration complete, results available
 *
 * Usage:
 * @code
 *   InputShaperCalibrator calibrator(api);
 *
 *   calibrator.check_accelerometer([](float noise) {
 *       // Accelerometer OK, noise level acceptable
 *   });
 *
 *   calibrator.run_calibration('X',
 *       [](int pct) { update_progress(pct); },
 *       [](const InputShaperResult& r) { show_result(r); },
 *       [](const std::string& err) { show_error(err); });
 * @endcode
 */
class InputShaperCalibrator {
  public:
    /**
     * @brief Calibrator state machine states
     */
    enum class State {
        IDLE,          ///< Ready to start, no calibration in progress
        CHECKING_ADXL, ///< Checking accelerometer connectivity
        TESTING_X,     ///< Running resonance test on X axis
        TESTING_Y,     ///< Running resonance test on Y axis
        READY          ///< Calibration complete, results available
    };

    /**
     * @brief Results container for both axes
     */
    struct CalibrationResults {
        InputShaperResult x_result; ///< X axis calibration result
        InputShaperResult y_result; ///< Y axis calibration result
        float noise_level = 0.0f;   ///< Measured accelerometer noise level

        /**
         * @brief Check if X axis result is valid
         */
        [[nodiscard]] bool has_x() const {
            return x_result.is_valid();
        }

        /**
         * @brief Check if Y axis result is valid
         */
        [[nodiscard]] bool has_y() const {
            return y_result.is_valid();
        }

        /**
         * @brief Check if both axes have valid results
         */
        [[nodiscard]] bool is_complete() const {
            return has_x() && has_y();
        }
    };

    /**
     * @brief Default constructor for tests without API
     *
     * Operations will fail with error callbacks when no API is available.
     */
    InputShaperCalibrator();

    /**
     * @brief Constructor with API dependency injection
     *
     * @param api Non-owning pointer to MoonrakerAPI instance
     */
    explicit InputShaperCalibrator(MoonrakerAPI* api);

    /**
     * @brief Destructor
     */
    ~InputShaperCalibrator() = default;

    // Non-copyable, movable
    InputShaperCalibrator(const InputShaperCalibrator&) = delete;
    InputShaperCalibrator& operator=(const InputShaperCalibrator&) = delete;
    InputShaperCalibrator(InputShaperCalibrator&&) = default;
    InputShaperCalibrator& operator=(InputShaperCalibrator&&) = default;

    /**
     * @brief Get current calibrator state
     * @return Current state
     */
    [[nodiscard]] State get_state() const {
        return state_;
    }

    /**
     * @brief Check accelerometer connectivity and measure noise level
     *
     * Runs MEASURE_AXES_NOISE to verify accelerometer is working and
     * measure background vibration level.
     *
     * @param on_complete Called with noise level on success
     * @param on_error Called with error message on failure
     */
    void check_accelerometer(AccelCheckCallback on_complete, ErrorCallback on_error = nullptr);

    /**
     * @brief Run resonance calibration on specified axis
     *
     * Executes SHAPER_CALIBRATE for the specified axis, collecting
     * frequency response data and all fitted shaper alternatives.
     *
     * @param axis Axis to test ('X' or 'Y')
     * @param on_progress Called with percentage (0-100) during test
     * @param on_complete Called with calibration result on success
     * @param on_error Called with error message on failure
     */
    void run_calibration(char axis, ProgressCallback on_progress, ResultCallback on_complete,
                         ErrorCallback on_error);

    /**
     * @brief Cancel any in-progress calibration
     *
     * Aborts current test and returns to IDLE state.
     * Safe to call even if no calibration is running.
     *
     * NOTE: This only resets local state. Once SHAPER_CALIBRATE or
     * MEASURE_AXES_NOISE has been dispatched, Klipper will run it to
     * completion regardless. Use emergency_abort() to actually stop the
     * macro on the printer side.
     */
    void cancel() {
        state_ = State::IDLE;
    }

    /**
     * @brief True iff a calibration command has been dispatched and not
     *        yet completed (CHECKING_ADXL / TESTING_X / TESTING_Y).
     */
    [[nodiscard]] bool is_active() const {
        return state_ == State::CHECKING_ADXL || state_ == State::TESTING_X ||
               state_ == State::TESTING_Y;
    }

    /**
     * @brief Emergency-abort an in-progress calibration on the printer.
     *
     * Sends M112 followed by firmware_restart — the only reliable way to
     * stop SHAPER_CALIBRATE / TEST_RESONANCES once Klipper has started
     * the macro (it blocks the gcode queue, so CANCEL_PRINT won't help).
     * Fire-and-forget; logs success/failure. Sets local state to IDLE.
     *
     * Callers should suppress the EmergencyStopOverlay recovery dialog
     * and the disconnect modal *before* invoking this, since M112 +
     * restart triggers both.
     */
    void emergency_abort();

    /**
     * @brief Get stored calibration results
     * @return Reference to results container
     */
    [[nodiscard]] const CalibrationResults& get_results() const {
        return results_;
    }

    /**
     * @brief Apply input shaper settings to printer
     *
     * Sends SET_INPUT_SHAPER command with specified configuration.
     *
     * @param config Settings to apply
     * @param on_success Called on successful application
     * @param on_error Called with error message on failure
     */
    void apply_settings(const ApplyConfig& config, SuccessCallback on_success,
                        ErrorCallback on_error);

    /**
     * @brief Save current input shaper settings to printer.cfg
     *
     * Sends SAVE_CONFIG to persist settings across restarts.
     *
     * @param on_success Called on successful save
     * @param on_error Called with error message on failure
     */
    void save_to_config(SuccessCallback on_success, ErrorCallback on_error);

  private:
    /**
     * @brief Ensure printer is homed before proceeding
     *
     * Checks homed_axes subject. If not fully homed, sends G28 first.
     * Calls continuation on success, on_error on failure.
     *
     * @param then Callback to invoke once homing is confirmed
     * @param on_error Called with error message if homing fails
     */
    void ensure_homed_then(std::function<void()> then, ErrorCallback on_error);

    MoonrakerAPI* api_ = nullptr; ///< Non-owning pointer to API
    State state_ = State::IDLE;
    CalibrationResults results_;
};

} // namespace calibration
} // namespace helix

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::sensors {

/// @brief Role assigned to a temperature sensor (auto-categorized during discovery)
enum class TemperatureSensorRole {
    NONE = 0,           ///< Discovered but not assigned to a role
    CHAMBER = 1,        ///< Chamber temperature monitoring
    MCU = 2,            ///< MCU/board temperature
    HOST = 3,           ///< Host computer (Raspberry Pi, etc.)
    AUXILIARY = 4,      ///< Any other temperature sensor
    STEPPER_DRIVER = 5, ///< TMC stepper driver built-in temperature
};

/// @brief Type of temperature sensor in Klipper
enum class TemperatureSensorType {
    TEMPERATURE_SENSOR = 1, ///< temperature_sensor (read-only)
    TEMPERATURE_FAN = 2,    ///< temperature_fan (has target and speed)
};

/// @brief Configuration for a temperature sensor
struct TemperatureSensorConfig {
    std::string klipper_name; ///< Full Klipper name (e.g., "temperature_sensor mcu_temp")
    std::string sensor_name;  ///< Short name (e.g., "mcu_temp")
    std::string display_name; ///< Pretty name (e.g., "MCU Temperature")
    TemperatureSensorType type = TemperatureSensorType::TEMPERATURE_SENSOR;
    TemperatureSensorRole role = TemperatureSensorRole::NONE; ///< Auto-assigned during discovery
    bool enabled = true;
    int priority = 100; ///< Lower = shown first

    TemperatureSensorConfig() = default;

    TemperatureSensorConfig(std::string klipper_name_, std::string sensor_name_,
                            std::string display_name_, TemperatureSensorType type_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          display_name(std::move(display_name_)), type(type_) {}
};

/// @brief Runtime state for a temperature sensor
struct TemperatureSensorState {
    float temperature = 0.0f; ///< Temperature in degrees C
    float target = 0.0f;      ///< Target temp (temperature_fan only)
    float speed = 0.0f;       ///< Fan speed 0-1 (temperature_fan only)
    bool available = false;   ///< Sensor available in current config
};

/// @brief Convert role enum to config string
/// @param role The role to convert
/// @return Config-safe string for JSON storage
[[nodiscard]] inline std::string temp_role_to_string(TemperatureSensorRole role) {
    switch (role) {
    case TemperatureSensorRole::NONE:
        return "none";
    case TemperatureSensorRole::CHAMBER:
        return "chamber";
    case TemperatureSensorRole::MCU:
        return "mcu";
    case TemperatureSensorRole::HOST:
        return "host";
    case TemperatureSensorRole::AUXILIARY:
        return "auxiliary";
    case TemperatureSensorRole::STEPPER_DRIVER:
        return "stepper_driver";
    default:
        return "none";
    }
}

/// @brief Parse role string to enum
/// @param str The config string to parse
/// @return Parsed role, or NONE if unrecognized
[[nodiscard]] inline TemperatureSensorRole temp_role_from_string(const std::string& str) {
    if (str == "chamber")
        return TemperatureSensorRole::CHAMBER;
    if (str == "mcu")
        return TemperatureSensorRole::MCU;
    if (str == "host")
        return TemperatureSensorRole::HOST;
    if (str == "auxiliary")
        return TemperatureSensorRole::AUXILIARY;
    if (str == "stepper_driver")
        return TemperatureSensorRole::STEPPER_DRIVER;
    return TemperatureSensorRole::NONE;
}

/// @brief Convert role to display string
/// @param role The role to convert
/// @return Human-readable role name for UI display
[[nodiscard]] inline std::string temp_role_to_display_string(TemperatureSensorRole role) {
    switch (role) {
    case TemperatureSensorRole::NONE:
        return "Unassigned";
    case TemperatureSensorRole::CHAMBER:
        return "Chamber";
    case TemperatureSensorRole::MCU:
        return "MCU";
    case TemperatureSensorRole::HOST:
        return "Host";
    case TemperatureSensorRole::AUXILIARY:
        return "Auxiliary";
    case TemperatureSensorRole::STEPPER_DRIVER:
        return "Stepper Driver";
    default:
        return "Unassigned";
    }
}

/// @brief Convert type enum to config string
/// @param type The type to convert
/// @return Config-safe string
[[nodiscard]] inline std::string temp_type_to_string(TemperatureSensorType type) {
    switch (type) {
    case TemperatureSensorType::TEMPERATURE_SENSOR:
        return "temperature_sensor";
    case TemperatureSensorType::TEMPERATURE_FAN:
        return "temperature_fan";
    default:
        return "temperature_sensor";
    }
}

/// @brief Parse type string to enum
/// @param str The config string to parse
/// @return Parsed type, defaults to TEMPERATURE_SENSOR if unrecognized
[[nodiscard]] inline TemperatureSensorType temp_type_from_string(const std::string& str) {
    if (str == "temperature_fan")
        return TemperatureSensorType::TEMPERATURE_FAN;
    return TemperatureSensorType::TEMPERATURE_SENSOR;
}

} // namespace helix::sensors

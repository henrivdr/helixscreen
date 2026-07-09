// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "moonraker_client.h" // for helix::ConnectionState, helix::SubscriptionId
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>
#include <vector>

namespace helix {
struct SensorInfo; // Forward declaration for get_sensors()
} // namespace helix

/**
 * @brief Abstract interface for the high-level Moonraker API façade.
 *
 * Production and test consumers that only need polymorphic access to the
 * Moonraker domain layer should depend on this interface rather than the
 * concrete MoonrakerAPI.
 *
 * The interface is intentionally narrow — it mirrors only the methods
 * currently marked `virtual` on MoonrakerAPI. Non-virtual helpers
 * (set_temperature, execute_gcode, exclude_object, etc.) and sub-API
 * accessors (advanced(), files(), ...) remain on the concrete class.
 * MoonrakerAPIMock still inherits the concrete MoonrakerAPI so it can
 * reuse those non-virtual helpers and sub-API composition; drift
 * protection is still enforced through the inheritance chain.
 */
class IMoonrakerAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using BoolCallback = std::function<void(bool)>;
    using StringCallback = std::function<void(const std::string&)>;
    using JsonCallback = std::function<void(const json&)>;

    using PowerDevicesCallback = std::function<void(const std::vector<PowerDevice>&)>;
    using SensorsCallback =
        std::function<void(const std::vector<helix::SensorInfo>&, const nlohmann::json&)>;

    virtual ~IMoonrakerAPI() = default;

    // ========================================================================
    // Power Device Control
    // ========================================================================

    /// @brief Get list of all configured power devices
    virtual void get_power_devices(PowerDevicesCallback on_success, ErrorCallback on_error) = 0;

    /// @brief Set power device state ("on", "off", "toggle")
    virtual void set_device_power(const std::string& device, const std::string& action,
                                  SuccessCallback on_success, ErrorCallback on_error) = 0;

    // ========================================================================
    // Sensor Operations
    // ========================================================================

    /// @brief Get list of all configured Moonraker sensors
    virtual void get_sensors(SensorsCallback on_success, ErrorCallback on_error = nullptr) = 0;

    // ========================================================================
    // Connection State
    // ========================================================================

    /// @brief Check if the client is currently connected to Moonraker
    virtual bool is_connected() const = 0;

    /// @brief Get current connection state
    virtual helix::ConnectionState get_connection_state() const = 0;

    /// @brief Get the WebSocket URL used for the current connection
    virtual std::string get_websocket_url() const = 0;

    // ========================================================================
    // Subscriptions and Method Callbacks
    // ========================================================================

    /// @brief Subscribe to status update notifications
    virtual helix::SubscriptionId
    subscribe_notifications(std::function<void(const json&)> callback) = 0;

    /// @brief Unsubscribe from status update notifications
    virtual bool unsubscribe_notifications(helix::SubscriptionId id) = 0;

    /// @brief Register a persistent callback for a specific notification method
    virtual void register_method_callback(const std::string& method, const std::string& name,
                                          std::function<void(const json&)> callback) = 0;

    /// @brief Unregister a method-specific callback
    virtual bool unregister_method_callback(const std::string& method, const std::string& name) = 0;

    /// @brief Temporarily suppress disconnect modal notifications
    virtual void suppress_disconnect_modal(uint32_t duration_ms) = 0;

    /// @brief Retrieve recent G-code commands/responses from Moonraker's store
    virtual void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    std::function<void(const MoonrakerError&)> on_error) = 0;

    // ========================================================================
    // Helix Plugin
    // ========================================================================

    /// @brief Get phase tracking plugin status
    virtual void get_phase_tracking_status(std::function<void(bool enabled)> on_success,
                                           ErrorCallback on_error = nullptr) = 0;

    /// @brief Enable or disable phase tracking plugin
    virtual void set_phase_tracking_enabled(bool enabled,
                                            std::function<void(bool success)> on_success,
                                            ErrorCallback on_error = nullptr) = 0;

    // ========================================================================
    // Moonraker Database
    // ========================================================================

    /// @brief Get a value from Moonraker's database
    virtual void database_get_item(const std::string& namespace_name, const std::string& key,
                                   std::function<void(const json&)> on_success,
                                   ErrorCallback on_error = nullptr) = 0;

    /// @brief Store a value in Moonraker's database
    virtual void database_post_item(const std::string& namespace_name, const std::string& key,
                                    const json& value, std::function<void()> on_success = nullptr,
                                    ErrorCallback on_error = nullptr) = 0;

    /// @brief Get all keys in a namespace. Moonraker returns a JSON object
    ///        mapping key -> value. Empty object if namespace is empty/missing.
    virtual void database_get_namespace(const std::string& namespace_name,
                                        std::function<void(const json&)> on_success,
                                        ErrorCallback on_error = nullptr) = 0;

    /// @brief Delete a key from Moonraker's database. Implementations normalize
    ///        the "missing key" error (Moonraker returns 404 for absent entries)
    ///        into on_success so callers can treat delete as idempotent. on_error
    ///        only fires for genuine failures (network, permission, protocol).
    virtual void database_delete_item(const std::string& namespace_name, const std::string& key,
                                      std::function<void()> on_success = nullptr,
                                      ErrorCallback on_error = nullptr) = 0;
};

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_types.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

/**
 * @file ams_backend_ace.h
 * @brief ACE (Anycubic ACE Pro) backend implementation
 *
 * Implements the AmsBackend interface for AnyCubic ACE Pro systems
 * using the ValgACE/BunnyACE/DuckACE Klipper drivers.
 *
 * Primary path (ValgACE): Subscribes to the `ace` Klipper object via
 * standard Moonraker WebSocket. ValgACE implements get_status() which
 * returns combined state in a single object.
 *
 * Fallback path (BunnyACE/DuckACE): If the initial query returns empty
 * data (driver lacks get_status()), falls back to REST polling via the
 * ace_status.py Moonraker bridge at /server/ace/ endpoints.
 *
 * G-code Commands:
 * - ACE_CHANGE_TOOL TOOL={n}  - Load filament from slot n (-1 to unload)
 * - ACE_START_DRYING TEMP={t} DURATION={m}  - Start drying
 * - ACE_STOP_DRYING           - Stop drying
 *
 * Thread Model:
 * - Primary: WebSocket subscription callbacks on background thread
 * - Fallback: REST polling thread at ~500ms interval
 * - State is cached under mutex protection (inherited from AmsSubscriptionBackend)
 */
class AceTestAccess;

class AmsBackendAce : public AmsSubscriptionBackend {
  public:
    AmsBackendAce(MoonrakerAPI* api, helix::MoonrakerClient* client);

    ~AmsBackendAce() override;

    // ========================================================================
    // Type
    // ========================================================================

    [[nodiscard]] AmsType get_type() const override { return AmsType::ACE; }

    // ACE has no internal tool-routing table; gcode Tx commands must be
    // rewritten with the user's slot->tool assignment before sending.
    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::GcodeRewrite;
    }

    // ========================================================================
    // State Queries
    // ========================================================================

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // ========================================================================
    // Path Visualization
    // ========================================================================

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // ========================================================================
    // Filament Operations
    // ========================================================================

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // ========================================================================
    // Recovery Operations
    // ========================================================================

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and fires
    // override_store_->clear_async. ACE firmware doesn't populate brand /
    // spool_name / spoolman_* / weights / color_name — those are override-only,
    // so they're all zeroed. Color/material come from firmware so they stay.
    // The hardware-event detector calls this internally once an EMPTY -> present
    // transition confirms a physical swap.
    void clear_slot_override(int slot_index) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // ACE has fixed 1:1 mapping (tools ARE slots), not configurable
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // ========================================================================
    // Bypass Mode (not supported on ACE Pro)
    // ========================================================================

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // ========================================================================
    // Environment Sensors & Dryer Control (ACE Pro has built-in dryer + temp)
    // ========================================================================

    [[nodiscard]] bool has_environment_sensors() const override { return true; }
    [[nodiscard]] DryerInfo get_dryer_info() const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1,
                           int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1) override;
    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override;

    // ========================================================================
    // Device Actions
    // ========================================================================

    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    // ========================================================================
    // AmsSubscriptionBackend hooks
    // ========================================================================

    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[ACE]"; }
    void on_started() override;
    void on_stopping() override;

    // ========================================================================
    // Response Parsing (protected for unit testing)
    // ========================================================================

    /**
     * @brief Parse /server/ace/info response (REST fallback path)
     * @param data JSON response data
     */
    void parse_info_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/status response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_status_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/slots response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_slots_response(const nlohmann::json& data);

    /**
     * @brief Parse combined ace Klipper object data (WebSocket subscription path)
     *
     * ValgACE's get_status() returns all state in one object: model, firmware,
     * status, slots, dryer, etc. This handles that combined format.
     *
     * @param data JSON data from the ace Klipper object
     */
    void parse_ace_object(const nlohmann::json& data);

  private:
    friend class ::AceTestAccess;

    // ========================================================================
    // REST Fallback (for BunnyACE/DuckACE without get_status())
    // ========================================================================

    void start_rest_fallback();
    void stop_rest_fallback();
    void rest_polling_loop();

    void poll_info();
    void poll_status();
    void poll_slots();

    // ========================================================================
    // Helpers
    // ========================================================================

    AmsError validate_slot_index(int slot_index) const;

    /**
     * @brief Parse slot color from either RGB array [r,g,b] or hex string "#RRGGBB"
     * @param color_val JSON value (array or string)
     * @return Parsed RGB color value
     */
    static uint32_t parse_slot_color(const nlohmann::json& color_val);

    // ========================================================================
    // Members
    // ========================================================================

    // Dryer state (ACE-specific, not in base class)
    DryerInfo dryer_info_;

    // Info tracking
    std::atomic<bool> info_fetched_{false};
    std::atomic<int> info_fetch_failures_{0};

    // Callback lifetime management
    helix::AsyncLifetimeGuard lifetime_;

    // REST fallback state
    bool use_rest_fallback_{false};
    std::thread rest_polling_thread_;
    std::atomic<bool> rest_stop_requested_{false};
    std::condition_variable rest_stop_cv_;
    std::mutex rest_stop_mutex_;

    // Configuration
    static constexpr int POLL_INTERVAL_MS = 500;

    // Layer any configured FilamentSlotOverride for `slot_index` over `slot`,
    // mutating `slot` in place. Override wins for every non-default field;
    // default values (empty string, 0, -1.0 weights) fall through to the
    // firmware-reported data untouched. Called from parse_ace_object so every
    // parse path picks up the override before the SlotInfo is exposed via
    // events. ACE hardware doesn't carry brand/spool/weights, so the override
    // is the only source for those fields; color/material come from both the
    // firmware and user edits and the override wins per the merge policy.
    void apply_overrides(SlotInfo& slot, int slot_index);

    // Hardware-event detection: ACE has no RFID UID, so "user physically
    // swapped the spool" is inferred from a status transition EMPTY -> present
    // (AVAILABLE / LOADED). When detected, the stored override for the slot
    // is cleared so stale brand/spool_name/spoolman_id from the previous
    // spool don't bleed onto the new one. Override-exclusive fields on `slot`
    // are zeroed in place so the cleared state is visible in the very next
    // get_slot_info() read (apply_overrides then no-ops for that slot).
    //
    // Called from parse_ace_object BEFORE apply_overrides, so the check
    // decides based on parsed firmware status (not override-masked data). The
    // caller is responsible for skipping the very first observation (no prior
    // prev_slot_status_ entry) — first-observation is a baseline and never
    // fires. Limitation: a LOADED -> EMPTY -> LOADED sequence (user unloaded
    // and reinserted the same spool) looks identical to a swap under this
    // status-based heuristic and clears the override. Documented tradeoff —
    // ACE's single signal is too coarse to distinguish the two cases.
    void check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                    SlotStatus previous_status, SlotStatus current_status);

    // Shared helper used by every override-clear path (hardware event and
    // explicit user request). Caller must hold mutex_. Erases
    // overrides_[slot_index], resets override-exclusive fields on the
    // provided SlotInfo (brand, spool_name, spoolman_*, weights, color_name),
    // and fires clear_async. Color/material stay untouched — firmware owns
    // them for ACE and the parse has just refreshed them.
    void clear_override_locked(int slot_index, SlotInfo& slot);

    // User-provided per-slot metadata (brand, spool name, spoolman IDs,
    // remaining weight, etc.) layered over firmware-reported state.
    // Both writers (on_started initial load, set_slot_info persist path) hold
    // mutex_; apply_overrides reads inside the parse path under mutex_.
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Previous slot status per slot index. Used as the swap-detection signal:
    // an EMPTY -> present transition fires the clear-override path. Map
    // presence also acts as the baseline guard: absent entry means "no prior
    // observation" and the check is skipped (first observation never clears).
    // Access is always under mutex_ (parse_ace_object is the only
    // writer/reader).
    std::unordered_map<int, SlotStatus> prev_slot_status_;
};

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_IFS

#include "ams_subscription_backend.h"
#include "error_event.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "slot_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Ad5xIfsTestAccess;

/// AMS backend for FlashForge Adventurer 5X IFS (Intelligent Filament Switching).
///
/// IFS is a 4-lane filament switching system controlled by a separate STM32 MCU,
/// driven through ZMOD's zmod_ifs.py Klipper module.
///
/// === Stock zMod vs plugin variants (IMPORTANT for Moonraker visibility) ===
///
/// Stock zMod owns two Klipper objects, `zmod_ifs` and `zmod_color`, that hold
/// the authoritative per-channel state:
///   - zmod_ifs.ifs_data.get_port(port)         -> per-channel HUB presence switch
///   - zmod_ifs.get_ifs_sensor(port)            -> per-channel motion/stall sensor
///                                                 (located INSIDE the IFS, just
///                                                 after the hub — NOT at the toolhead)
///   - zmod_ifs.get_extruder_sensor()           -> toolhead filament switch
///   - zmod_ifs.get_prutok_type_from_config(p)  -> per-channel material string
///   - zmod_color.get_current_channel()         -> active channel (1-based)
///   - zmod_color.get_printer_data_detail()     -> hasMatlStation, indepMatlInfo, ...
///
/// These are `printer.lookup_object()`-only Python APIs. They are NOT exposed via
/// `get_status()`, so Moonraker (and therefore HelixScreen) cannot subscribe to
/// them directly. Stock zMod only gives Moonraker:
///   - filament_motion_sensor ifs_motion_sensor   (single boolean, post-hub)
///   - filament_switch_sensor head_switch_sensor  (toolhead)
///   - Adventurer5M.json                          (polled via Moonraker file API)
///
/// The lessWaste / bambufy plugins close this gap. They are effectively a
/// Moonraker exporter for zmod_ifs/zmod_color, publishing:
///   - filament_switch_sensor _ifs_port_sensor_{1-4}  per-port HUB presence
///     (wraps zmod_ifs.ifs_data.get_port)
///   - save_variables with <prefix>_colors, _types, _tools, _current_tool,
///     _external   (prefix = "less_waste" or "bambufy"; schema identical)
///   - _IFS_VARS gcode macro for atomic writes of the above
///
/// Plugin delta over stock zMod (via Moonraker):
///   (1) per-channel HUB presence as 4 separate booleans
///   (2) live tool->port mapping (16 slots)
///   (3) active tool index with push notifications
///   (4) bypass/external flag
///   (5) atomic, subscribable color+material updates
/// Everything else — including the toolhead switch — is shared with stock zMod.
///
/// === Sensor -> PathSegment mapping ===
///
///   head_switch_sensor        -> TOOLHEAD / NOZZLE (at toolhead)
///   _ifs_port_sensor_{1..4}   -> HUB               (per-channel, plugin only)
///   ifs_motion_sensor         -> OUTPUT            (post-hub, NOT toolhead;
///                                                   single boolean on stock zMod)
///
/// NOTE: `parse_head_sensor()` currently conflates `ifs_motion_sensor` with the
/// toolhead switch. That is a known simplification — motion at the hub does not
/// mean filament has reached the nozzle. Fixing this requires splitting a
/// hub_output presence from head_filament and updating
/// `system_info_.filament_loaded` + `detect_load_unload_completion()` accordingly.
///
/// Ports are 1-based (1-4), slots are 0-based (0-3).
/// slot_to_port = slot + 1, port_to_slot = port - 1.
class AmsBackendAd5xIfs : public AmsSubscriptionBackend {
  public:
    AmsBackendAd5xIfs(MoonrakerAPI* api, helix::MoonrakerClient* client);
    ~AmsBackendAd5xIfs() override;

    static constexpr int NUM_PORTS = 4;
    static constexpr int TOOL_MAP_SIZE = 16;
    static constexpr int UNMAPPED_PORT = 5;

    /**
     * @brief Bare filament-sensor names AD5X IFS owns.
     *
     * Native ZMOD post-hub motion sensor ifs_motion_sensor; toolhead
     * head_switch_sensor; lessWaste per-port _ifs_port_sensor_N; older ZMOD
     * _ifs_motion_sensor_N. Static and discovery-free; @p discovery is accepted
     * for signature uniformity. See AmsBackend::sensor_belongs_to_backend (#1054).
     */
    static bool owns_filament_sensor(const std::string& bare_name,
                                     const helix::PrinterDiscovery& discovery);

    // --- AmsBackend interface ---
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::AD5X_IFS;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::LINEAR;
    }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] bool is_bypass_active() const override;

    // Unload-action gate (also suppresses Load for the same slot). Keeps the
    // firmware's active slot unloadable even when a runout has cleared the head
    // sensor and dropped the slot's display status below LOADED — that runout is
    // precisely when the user needs Unload (#995). The same flag keeps Load
    // hidden on the active slot, which is intended: don't offer Load on a slot
    // the firmware still considers seated.
    [[nodiscard]] bool can_unload_from_toolhead(int slot_index) const override;

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // Cold per-lane eject / recover (#996). Issues `IFS_F11 PRUTOK={port}
    // CHECK=0` — a cold retract that drives one idle lane's feed motor backward
    // toward the spool. It does NOT heat the hotend and (CHECK=0) ignores the
    // lane presence/runout sensor, so it recovers a snapped chunk stuck in an
    // idle lane. Refuses the slot currently loaded to the toolhead (use Unload
    // first). Not routed through ensure_homed_then() — no toolhead move.
    AmsError eject_lane(int slot_index) override;
    [[nodiscard]] bool supports_lane_eject() const override {
        return true;
    }
    [[nodiscard]] bool supports_force_eject() const override {
        return true;
    }

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    [[nodiscard]] std::optional<helix::ErrorEvent> current_error() const override;

    // Backend-driven step model for the right-side vertical operation tracker.
    // The AD5X synthesizes 3 firmware phases (HEATING→CUTTING→UNLOADING /
    // HEATING→LOADING→PURGING) from extruder temp + head sensor in
    // apply_phase_action_locked(); these expose them as labelled steps + the
    // current-step subject so the tracker advances instead of falling back to
    // the legacy coarse AmsAction model.
    [[nodiscard]] OperationStepModel
    get_operation_step_model(StepOperationType op) const override;
    [[nodiscard]] lv_subject_t* get_operation_step_index_subject(StepOperationType op) override;

    // User-initiated state refresh. Re-reads Adventurer5M.json (the JSON poll
    // is the primary truth source on both old and new zmod) and schedules a
    // GET_ZCOLOR SILENT=1 follow-up to refresh the active-slot view. Both
    // calls are debounced/coalesced internally so this is safe to invoke
    // from screen-activation hooks.
    void request_resync() override;

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    // Weight-only persist: updates remaining/total weight in the override store
    // and NEVER rewrites Adventurer5M.json / _IFS_VARS or re-locks material —
    // the firmware-facing writers in set_slot_info() are what reverted the
    // user's material on every 60 s consumption persist (#981).
    void update_slot_weight(int slot_index, float remaining_weight_g, float total_weight_g,
                            bool persist) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Tool reassignment is only persistable when the lessWaste/bambufy plugin
    // is loaded (has_ifs_vars_): `_IFS_VARS tools=...` writes save_variables
    // that the plugin replays at print start. On native ZMOD the macro is
    // absent and set_tool_mapping() can only mutate local state, which the
    // firmware ignores — surface that as caps={false,false} so the print
    // start path doesn't silently drop user-supplied remaps.
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // Explicit user-initiated override clear (e.g. "Clear slot metadata" button
    // in the AMS edit modal). Erases overrides_[slot_index], resets the
    // override-exclusive fields on the live SlotInfo, and kicks off
    // override_store_->clear_async so the Moonraker lane_data entry is deleted.
    // The eject path (parse_adventurer_json detecting an empty ffmColor while
    // port_presence was true) shares this routine so the field-reset policy
    // stays in one place.
    void clear_slot_override(int slot_index) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;

    // IFS firmware persists color + material type but NOT spoolman_id,
    // so ToolState must handle spool assignment persistence via Moonraker DB.
    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return false;
    }

    [[nodiscard]] RemapStrategy get_remap_strategy() const override {
        return RemapStrategy::Native;
    }

    // IFS retracts filament from the extruder at end-of-print by default, so
    // the toolhead is expected to be empty at the next print-start. Suppresses
    // the pre-print runout warning modal.
    [[nodiscard]] bool auto_unloads_after_print() const override {
        return true;
    }

    // AD5X IFS firmware (ZMOD) validates material against a fixed whitelist
    // and rejects anything outside it with "Invalid material type: X. Valid: ...".
    // The UI dropdown is filtered to this list and outgoing values are normalized
    // via normalize_material() before being sent to firmware.
    [[nodiscard]] std::optional<std::vector<std::string>> get_supported_materials() const override;

    // Firmware-specific aliases for the shared normalize_material() pipeline.
    // AD5X treats SILK as distinct from PLA, but the shared filament DB
    // groups silk variants under compat_group "PLA" (most printers don't
    // make that distinction), so without these aliases "Silk PLA" would
    // collapse to "PLA" instead of "SILK".
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    get_material_aliases() const override;

    // Parse `[zmod_ifs] filament_<NAME>: <TEMP>` lines out of a user.cfg body.
    // Returns the NAME tokens (filament type tags as written, e.g. "PLA+").
    // Stateless + public so tests and external callers can drive it directly.
    static std::vector<std::string> parse_user_cfg_filament_types(const std::string& body);

    // Result of parsing a GET_ZCOLOR SILENT=1 response. Public so tests can
    // construct instances via Ad5xIfsTestAccess.
    struct ZColorSlot {
        std::string material;
        std::string hex; // Empty for old-format zmod responses (no /HEX).
    };

    struct ZColorSilentResult {
        bool is_prompt_fallback = false; // Response was an action:prompt dialog
        bool is_old_format = false;      // Slot lines had no /HEX segment
        bool ifs_active = false;         // "IFS: True" in summary line
        bool saw_valid_response = false; // Matched at least one summary or slot line
        std::optional<int> current_channel;
        std::optional<int> extruder_slot; // 0-based, absent when "None"
        // Seated/engaged channel from IFS_STATUS "Chan" (1-based, 0 = none).
        // Distinct from current_channel (the stale "(N)" paren form, unused) and
        // from extruder_slot (the live "Extruder:" feed view, which reads "None"
        // while loaded-idle). Chan persists at the physically seated port, so it
        // is the seated-channel authority for active_tool_/current_slot.
        std::optional<int> ifs_chan;
        std::array<std::optional<ZColorSlot>, NUM_PORTS> slots;
    };

  protected:
    void on_started() override;
    void on_stopping() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS AD5X-IFS]";
    }

  private:
    friend class Ad5xIfsTestAccess;

    void parse_save_variables(const nlohmann::json& vars);
    void parse_port_sensor(int port_1based, bool detected);
    void parse_head_sensor(bool detected);
    // One-shot fetch of /mod_data/user.cfg. Parses the [zmod_ifs] section for
    // `filament_<NAME>: <TEMP>` entries — zmod's mechanism for user-defined
    // material types beyond the AD5X firmware whitelist (e.g., PLA+, RPLA,
    // HELIX). 404 → no-op (not a zmod printer or no user.cfg present).
    void fetch_user_cfg_materials();
    // One-shot fetch of /mod_data/filament.json — zmod's per-filament-type
    // table holding the unload tube length (filament_tube_length) and feed
    // speed (filament_ifs_speed) that eject_lane() needs for a real, full lane
    // retract. Mirrors read_adventurer_json's download_file + tok.defer
    // threading discipline. 404 → flip filament_json_supported_ false for the
    // session (not a zmod printer). filament.json changes rarely so this is a
    // one-shot fetch at startup/reconnect, NOT a periodic poll.
    void fetch_filament_json();
    // Pure parse helper (no IO) — populates filament_eject_params_ +
    // filament_eject_default_ from a filament.json body. Static-shaped but
    // mutates members under mutex_; exposed to tests via Ad5xIfsTestAccess.
    // Tolerant of malformed JSON (logs + leaves cache untouched).
    void parse_filament_json(const std::string& content);
    void update_slot_from_state(int slot_index);
    // Layer any configured FilamentSlotOverride for `slot_index` over `slot`,
    // mutating `slot` in place. Override wins for every non-default field;
    // default values (empty string, 0, -1.0 weights) fall through to the parsed
    // firmware data untouched. Called from update_slot_from_state so every
    // parse path (save_variables, Adventurer5M.json, GET_ZCOLOR SILENT=1) picks
    // up the override before the SlotInfo is exposed via events.
    void apply_overrides(SlotInfo& slot, int slot_index);
    // External-edit sync: if the firmware-reported color for `slot_index`
    // differs from the previously observed value, treat it as an external
    // color/material edit (Mainsail console, AD5X LCD, native zmod dialog,
    // CHANGE_ZCOLOR from any non-Helix path) and refresh the stored override
    // so Moonraker DB lane_data stays in sync with zmod truth. This is what
    // OrcaSlicer's MoonrakerPrinterAgent reads. The sync ONLY writes to
    // Moonraker DB via override_store_->save_async — it does NOT issue
    // CHANGE_ZCOLOR or write Adventurer5M.json, so it cannot trigger zmod's
    // material-confirmation popup.
    //
    // Sync policy (NOT clear — color change alone is NOT a physical-swap
    // signal, and treating it as one wiped lane_data every time the user
    // retitled a color via the printer LCD; bug surfaced via compulsivejohnny
    // on Discord):
    //   - Existing override: update color_rgb + material in place, fire save_async.
    //     brand/spool_name/spoolman_*/weights/color_name are PRESERVED.
    //   - No existing override: create a minimal one carrying just color +
    //     material (so lane_data has a record for Orca even when the user has
    //     never edited the slot via Helix), fire save_async.
    //   - Slot is empty (no filament — placeholder #808080 read from JSON):
    //     update baseline only, no sync. Eject is handled separately by
    //     parse_adventurer_json calling clear_override_locked directly.
    //
    // Called from update_slot_from_state BEFORE apply_overrides, so the check
    // sees firmware-truth (not the override-masked value). First observation
    // on a given slot is a baseline and never fires a sync.
    //
    // `observed_color = nullopt` is the explicit "no color reading" signal —
    // empty colors_[idx] (parse hasn't filled yet, transient JSON race). It
    // is ignored: never updates the baseline, never fires a sync. Pure black
    // (#000000 = 0x000000) is a legitimate user color and MUST be passed as
    // `std::optional<uint32_t>{0}`, not nullopt — the previous "0 = no signal"
    // sentinel silently dropped genuine black filament from external-edit
    // detection.
    //
    // Returns true if a sync was actually triggered (color delta detected,
    // slot has filament). Caller uses this to decide whether to push the
    // updated colors_/materials_ snapshot through _IFS_VARS so the
    // lessWaste/bambufy plugin's private save_variables track zmod truth.
    bool check_external_color_change(int slot_index, std::optional<uint32_t> observed_color,
                                     bool slot_has_filament);
    // Sync helper used by check_external_color_change. Caller must hold mutex_.
    // Updates an existing override's color_rgb + material, or creates a
    // minimal one if none exists. Fires save_async to push the result to the
    // Moonraker DB lane_data namespace. Returns true if anything actually
    // changed (i.e. save_async was issued); false on the in-sync short-circuit.
    bool sync_override_to_firmware_locked(int slot_index, uint32_t firmware_color,
                                          const std::string& firmware_material);
    // Shared helper for every override-clear path (eject detected in
    // parse_adventurer_json and explicit user request via clear_slot_override).
    // Caller must hold mutex_. Erases overrides_[slot_index], resets
    // override-exclusive fields on the provided SlotInfo (brand, spool_name,
    // spoolman_*, weights, color_name), and fires clear_async on the override
    // store. Firmware-sourced fields are left untouched.
    void clear_override_locked(int slot_index, SlotInfo& slot);
    void parse_adventurer_json(const std::string& content);
    void read_adventurer_json();
    void register_zcolor_listener();
    // Listener body — extracted so tests can drive it directly without a live
    // MoonrakerClient. Returns true if the line was buffered as part of an
    // in-flight GET_ZCOLOR response (i.e., it was NOT treated as an external
    // change trigger). Buffering-and-suppressing our own response avoids the
    // self-feedback spam loop that hit v0.99.51 (zmod's GET_ZCOLOR macro body
    // echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens which would otherwise re-arm
    // schedule_zcolor_query() at ~2-4 Hz).
    bool on_gcode_response_line(const std::string& line);
    void register_klippy_ready_listener();
    // Re-query `gcode_macro _ifs_vars` and update the latch + has_ifs_vars_.
    // Fired from notify_klippy_ready so a FIRMWARE_RESTART that adds or
    // removes the lessWaste/bambufy plugin macro doesn't leave us caching
    // the wrong has_ifs_vars_ for the rest of the helixscreen session.
    void recheck_ifs_vars_macro();
    void unregister_moonraker_listeners();
    void schedule_json_reread();
    // True when `content` differs from the last observed Adventurer5M.json
    // body. Updates last_json_content_ on change. Single source of truth for
    // the "did the JSON change?" decision used by both the initial read and
    // the periodic poll.
    bool note_json_content(const std::string& content);
    // Lightweight HTTP poll that downloads Adventurer5M.json and only fires
    // schedule_zcolor_query() when content actually changed. Replaces the old
    // unconditional 15s GET_ZCOLOR backstop — the JSON download is invisible
    // to the gcode console, so polling here costs nothing user-visible while
    // still catching native-dialog edits zmod makes outside our gcode path.
    void poll_adventurer_json();

    // GET_ZCOLOR SILENT=1 primary-truth query. zmod's Adventurer5M.json
    // is a stale last-known-colors cache; SILENT=1 emits one line per
    // physically loaded slot (filtered by live per-port sensors) plus a
    // summary line. See project_ifs_data_sources.md for rationale.
    void query_zcolor_silent();
    void schedule_zcolor_query(const char* reason = "unknown");
    void finalize_zcolor_response();
    void apply_zcolor_result(const ZColorSilentResult& result);
    static ZColorSilentResult parse_zcolor_silent(const std::vector<std::string>& lines,
                                                  const char* reason);

    std::string build_color_list_value() const;
    std::string build_type_list_value() const;
    std::string build_tool_map_value() const;
    AmsError write_ifs_var(const std::string& key, const std::string& value);
    AmsError write_adventurer_json(int slot_index);
    // Direct filesystem write to the resolved AD5X-stock-ZMOD config path. Used
    // when helix-screen runs on the same host as Moonraker AND the canonical
    // config path is present + writable; bypasses Moonraker's HTTP upload (which
    // does an os.rename across mount points on AD5X stock-ZMOD and corrupts the
    // file via EXDEV on the symlinked /usr/prog/config target). Returns
    // command_failed if the path isn't set or the read-modify-write fails.
    AmsError write_adventurer_json_local(int slot_index);
    // Resolve the on-disk Adventurer5M.json path when running on the same host
    // as Moonraker. Sets local_adventurer_json_path_ to the realpath of the
    // file if it exists and is regular; otherwise leaves it empty so we fall
    // back to the Moonraker upload path.
    void detect_local_adventurer_json_path();
    void detect_load_unload_completion(bool head_detected);

    // === Live load/unload progress phase tracker ===
    //
    // Between the moment WE start a load/unload (load_filament / unload_filament)
    // and the moment the operation finalizes (head transition completes, or the
    // action-timeout backstop fires), this tracker synthesizes the firmware's
    // internal phases from primary signals (extruder temp/target + head sensor)
    // and a secondary, corroborating parse of RESPOND lines. It overwrites
    // system_info_.action so the UI's existing step mapping advances correctly,
    // and sets a dynamic system_info_.operation_detail string.
    //
    // Sequences:
    //   Unload: HEATING → (temp ≥ target) CUTTING → (head drop) UNLOADING → IDLE
    //   Load:   HEATING → (temp ≥ target) LOADING → (head rise)  PURGING   → IDLE
    //
    // active gates the new behavior: when INACTIVE (legacy/external/firmware-
    // initiated action changes), detect_load_unload_completion preserves the
    // historical snap-to-IDLE on a head transition.
    struct IfsPhaseTracker {
        bool active = false;        // true between begin and finalize
        bool is_unload = false;     // unload vs load direction
        bool reached_target_once = false; // current temp ever within ~0.5°C of target
        bool seen_head_drop = false;      // head sensor true→false (cut/retract started)
        bool seen_head_rise = false;      // head sensor false→true (filament reached nozzle)
        int target_deci = 0;              // heat target in deci-degrees (×10), 0 = unknown
    };
    IfsPhaseTracker phase_tracker_;
    int last_extruder_temp_deci_ = 0;   // deci-degrees (×10)
    int last_extruder_target_deci_ = 0; // deci-degrees (×10)

    // Capture op-start state + set active. Caller must hold mutex_.
    void begin_phase_tracking_locked(bool is_unload);
    // Reset tracker (clears active). Caller must hold mutex_.
    void end_phase_tracking_locked();
    // Drive the phase machine on an extruder temp/target frame. Caller holds mutex_.
    void on_extruder_temp_locked(int temp_deci, int target_deci);
    // Drive the phase machine on a head-sensor transition. Caller holds mutex_.
    void on_head_transition_locked(bool detected);
    // Recompute system_info_.action + operation_detail + operation_phase from
    // tracker state. Caller must hold mutex_. Returns true when
    // system_info_.action actually changed — the caller releases mutex_ and
    // then emits EVENT_STATE_CHANGED (the lock contract requires emit_event with
    // the lock NOT held; see ams_subscription_backend.h). NOT emitting here is
    // what left the busy state unpublished until the next ~1.4s status frame.
    bool apply_phase_action_locked();
    // Set system_info_.operation_detail. Caller must hold mutex_.
    void set_operation_detail_locked(std::string detail);

    int find_first_tool_for_port(int port_1based) const;

    // Map active_tool_ -> system_info_.current_slot via tool_map_. Single source
    // of truth shared by handle_status_update and apply_zcolor_result so the
    // seated slot updates immediately when IFS_STATUS reports a new Chan instead
    // of waiting for the next status frame. Caller must hold mutex_.
    void recompute_current_slot_locked();

  private:
    bool validate_slot_index(int slot_index) const;
    void check_action_timeout();

    // Cached state from save_variables
    // Variable prefix: "less_waste" (lessWaste/zmod) or "bambufy" — auto-detected from
    // whichever save_variables are present on the printer.
    std::string var_prefix_ = "less_waste";
    std::array<std::string, NUM_PORTS> colors_;    // Hex strings: "FF0000"
    std::array<std::string, NUM_PORTS> materials_; // Material names: "PLA"

    // Per-filament-type eject parameters parsed from /mod_data/filament.json:
    // material name -> {filament_tube_length (LEN), filament_ifs_speed (SPEED)}.
    // Consumed by eject_lane() to drive a full per-material lane retract via
    // IFS_F11 LEN=.. SPEED=.. instead of the firmware default. Guarded by
    // mutex_. filament_eject_default_ holds the file's "default" entry (or the
    // hardcoded {1000, 1200} when absent) used when a lane's material is empty
    // or not present in the table.
    std::map<std::string, std::pair<int, int>> filament_eject_params_;
    std::pair<int, int> filament_eject_default_{1000, 1200};
    // filament.json availability latch (mirrors json_poll_supported_): starts
    // true, flips false permanently on a 404 so non-zmod printers stop fetching.
    std::atomic<bool> filament_json_supported_{true};
    // User-defined material types extending the firmware whitelist. Two
    // sources, both surfaced via get_supported_materials():
    //   - bambufy_custom_types in save_variables (when bambufy is/was active);
    //     parse_save_variables() populates this regardless of has_ifs_vars_
    //     because user-defined types are orthogonal to plugin activation.
    //   - [zmod_ifs] filament_<NAME>: <TEMP> in /mod_data/user.cfg (zmod's
    //     own mechanism); fetched once via fetch_user_cfg_materials() at
    //     backend start.
    // Guarded by its own mutex (NOT mutex_) so get_supported_materials() —
    // called from normalize_material() inside set_slot_info(), which already
    // holds mutex_ — doesn't deadlock. Both writers (parse_save_variables
    // and fetch_user_cfg_materials) currently take mutex_ AND
    // custom_types_mutex_; lock order is mutex_ → custom_types_mutex_.
    mutable std::mutex custom_types_mutex_;
    std::vector<std::string> custom_material_types_;
    std::array<int, TOOL_MAP_SIZE> tool_map_;   // tool_map_[tool] = port (1-4, 5=unmapped)
    std::array<bool, NUM_PORTS> port_presence_; // Per-port filament sensor state
    int active_tool_ = -1;                      // Current tool (-1 = none)
    // Physically seated port from IFS_STATUS "Chan" (1-4; 0 = none). Persists at
    // the seated port while loaded-idle (when GET_ZCOLOR's "Extruder:" reads
    // None), so it is the seated-channel authority for unload routing. Stored
    // unconditionally — independent of has_ifs_vars_ / tool_map_ — because the
    // tool_map_-derived current_slot can disagree with it on the plugin path.
    int seated_chan_ = 0;
    bool external_mode_ = false;                // Bypass/external spool mode
    bool head_filament_ = false;                // Head sensor state
    std::array<bool, NUM_PORTS> dirty_{};       // Per-slot dirty flag to prevent stale overwrites

    helix::printer::SlotRegistry slots_;

    // Native ZMOD IFS has no per-port sensors — infer port presence from active
    // tool + head sensor state so the UI doesn't show all slots as EMPTY.
    bool has_per_port_sensors_ = false;

    // True if _IFS_VARS macro is available (lessWaste or bambufy plugin).
    // False for native ZMOD, which stores color/type in Adventurer5M.json
    // (read/written via Moonraker HTTP file API).
    bool has_ifs_vars_ = false;

    // Latch: starts TRUE (pessimistic) — cleared when a `gcode_macro
    // _ifs_vars` query returns a non-empty variables dict (real macro
    // present). Prevents the race where a notify_status_update with
    // save_variables arrives between subscription registration and the
    // initial query callback, which would set has_ifs_vars_ = true before
    // we've verified the macro is loaded. Re-evaluated on every
    // notify_klippy_ready via recheck_ifs_vars_macro() so a FIRMWARE_RESTART
    // that adds/removes the macro takes effect without restarting
    // helixscreen — also forces has_ifs_vars_ = false when the macro goes
    // missing after a restart, and on Unknown-command responses to our own
    // _IFS_VARS writes (self-heal). Note: Klipper/Kalico return `{}` for
    // missing objects rather than erroring the query, so empty-vs-non-empty
    // is the discriminator, not key presence.
    bool ifs_macro_confirmed_missing_ = true;
    std::atomic<bool> reread_pending_{false};

    // Signature (count + per-slot color/material) of the slots parsed from the
    // last Adventurer5M.json read. Native ZMOD re-reads the file on every sensor
    // change / ~5s poll cycle; comparing against this lets the "Loaded N slots"
    // line log at INFO only when the parsed set actually changed.
    std::string last_parsed_signature_;

    // GET_ZCOLOR SILENT=1 query state.
    // zcolor_silent_supported_ starts optimistic; a prompt-style response
    // flips it false for the session (not retried).
    std::atomic<bool> zcolor_query_active_{false};
    std::atomic<bool> zcolor_query_pending_{false};
    // Coalesce-gate for schedule_zcolor_query(): true while a debounce worker is
    // in flight. zmod re-emits the "Select print materials" prompt on every
    // CHANGE_ZCOLOR, so a single color edit produces a burst of trigger lines —
    // without this gate each one submitted its own fast()-pool worker (20+ in a
    // 40ms window seen in bundle ACJRZBXJ), every one holding a pool slot through
    // its 500ms sleep while only a single query ever fired. zcolor_query_pending_
    // carries the "refresh wanted" signal, so later callers just set it and
    // return. Mirrors reread_pending_ on schedule_json_reread().
    std::atomic<bool> zcolor_schedule_armed_{false};
    std::atomic<bool> zcolor_silent_supported_{true};
    std::mutex zcolor_buffer_mutex_;
    std::vector<std::string> zcolor_response_buffer_;
    // Diagnostic counter — incremented on every schedule_zcolor_query() call.
    // Exposed via Ad5xIfsTestAccess so the listener-feedback regression test
    // can assert that buffered response lines never re-arm a query.
    std::atomic<uint32_t> zcolor_schedule_count_{0};
    // Diagnostic counter — incremented only when a debounce worker is actually
    // submitted (past the zcolor_schedule_armed_ gate). Lets the coalescing test
    // assert that a burst of triggers spawns a single worker, not one each.
    std::atomic<uint32_t> zcolor_worker_submit_count_{0};
    // Diagnostic-only: which operation triggered the next/current GET_ZCOLOR +
    // IFS_STATUS query, threaded into the IFS_STATUS Chan log line for field
    // diagnostics. const char* to string literals (no allocation). _pending_ is
    // set by schedule_zcolor_query(reason); _active_ is promoted from it when
    // query_zcolor_silent() actually fires. Main-thread only; not synchronized.
    const char* zcolor_query_reason_pending_ = "unknown";
    const char* zcolor_query_reason_active_ = "unknown";

    // JSON poll state: download Adventurer5M.json on a slow tick and compare
    // to last-seen content. Hash-by-equality is fine here — file is a few
    // hundred bytes and changes are rare. json_poll_supported_ flips false
    // permanently on a 404 so non-zmod printers stop trying.
    std::atomic<bool> json_poll_in_flight_{false};
    std::atomic<bool> json_poll_supported_{true};
    std::string last_json_content_; // protected by mutex_

    // Action timeout tracking. action_start_time_ is reset on every phase
    // transition (apply_phase_action_locked) so each phase gets its own window.
    // HEATING needs a longer budget: a real AD5X cold-start unload heats
    // ~26°C→230°C in ~158s (longer for high-temp materials approaching 300°C),
    // which far exceeds the 90s general timeout. 300s is a UI backstop only —
    // Klipper's own verify_heater aborts a genuinely stuck heater within ~1-2
    // min and the macro errors out, so this never gates real functionality.
    static constexpr int ACTION_TIMEOUT_SECONDS = 90;
    static constexpr int HEATING_TIMEOUT_SECONDS = 300;
    std::chrono::steady_clock::time_point action_start_time_;

    // Rate-limit gate for the JSON-content poll. handle_status_update kicks
    // poll_adventurer_json() if at least kJsonPollInterval has elapsed since
    // the last kick — replaces the old 15s unconditional GET_ZCOLOR backstop.
    // Default-constructed time_point is the epoch, so the first status update
    // after backend start fires a poll immediately.
    std::chrono::steady_clock::time_point last_json_poll_kick_{};

    // User-provided per-slot metadata (brand, spool name, spoolman IDs, remaining
    // weight, etc.) layered over firmware-reported state.
    //
    // Write paths (both hold mutex_):
    //   - on_started(): initial bulk load from Moonraker DB lane_data.
    //     Swap happens under mutex_ so a concurrent status notification can
    //     never see a torn map.
    //   - set_slot_info(persist=true): user edit staged into overrides_
    //     BEFORE update_slot_from_state() is called, so apply_overrides on
    //     the very same call applies the new values rather than the old
    //     pre-edit override.
    //
    // Read: in apply_overrides() during the parse path, which always runs
    // under mutex_ (via update_slot_from_state).
    std::unique_ptr<helix::ams::FilamentSlotOverrideStore> override_store_;
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides_;

    // Resolved on-disk path of Adventurer5M.json when helix-screen runs on the
    // same host as Moonraker. Empty string means "fall back to Moonraker HTTP
    // upload" — either we're remote, the file isn't where we expect it, or the
    // path isn't writable. Set once during on_started() via
    // detect_local_adventurer_json_path(); never mutated thereafter.
    std::string local_adventurer_json_path_;

    // Per-slot previous firmware color (NOT the override-masked value).
    // Used to detect external color/material edits (Mainsail console, AD5X
    // LCD, native zmod dialog) so we can refresh the Moonraker DB lane_data
    // entry that OrcaSlicer reads. Empty = first observation (baseline,
    // never triggers a sync). observed_color == 0 is ignored as "no reading"
    // and does not update the baseline.
    //
    // Startup safety: on_started() loads overrides_ from Moonraker DB BEFORE
    // any firmware parse runs, and last_firmware_color_ stays empty until the
    // first parse — so the startup window can't flag the initial observation
    // as an external edit. set_slot_info() also pre-updates this map with the
    // user's chosen color before calling update_slot_from_state() so a Helix-
    // initiated color edit isn't misread as a foreign one on the same call.
    //
    // Access is always under mutex_ (written/read from update_slot_from_state
    // -> check_external_color_change and from set_slot_info's pre-update, all
    // of which run under the lock).
    std::unordered_map<int, uint32_t> last_firmware_color_;

    // Bumped by sync_override_to_firmware_locked on every accepted external
    // edit (color or material delta detected for a present slot, lane_data
    // save_async issued). parse_adventurer_json snapshots the count around
    // its per-slot loop and uses the delta to decide whether to also mirror
    // colors_/materials_ into the lessWaste/bambufy plugin's _IFS_VARS
    // save_variables — those don't self-sync against zmod's
    // Adventurer5M.json, so without the mirror the plugin's runout-recovery
    // and smart-purge logic operate on stale data. Wraps on overflow which
    // is fine — the comparison is `>`, not equality. Always accessed under
    // mutex_.
    size_t external_sync_count_ = 0;

    // Note: uses inherited lifetime_ from AmsSubscriptionBackend (not shadowed).
};

#endif // HELIX_HAS_IFS

// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_snapmaker.h"

#include "ams_error.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api.h"
#include "pause_cause.h"
#include "post_op_cooldown_manager.h"
#include "snapmaker_resume.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <string_view>
#include <utility>

namespace {

// Snapmaker's recognized filament SUB_TYPE product lines. The RFID read path
// stores SUB_TYPE into SlotInfo::spool_name (see handle_status_update), but a
// user can edit spool_name to a free-form string ("My Custom Spool"). Both the
// set_slot_info firmware round-trip (POST /printer/filament_detect/set) and the
// #991 post-runout SET_PRINT_FILAMENT_CONFIG re-assert must only treat
// spool_name as a SUB_TYPE when it matches one of these — a single source of
// truth for "is this a real product line?".
constexpr std::array<std::string_view, 8> kKnownSubTypes = {
    "Basic", "Matte", "SnapSpeed", "Silk", "Support", "HF", "95A", "95A HF"};

[[nodiscard]] bool is_known_subtype(const std::string& s) {
    for (const auto& st : kKnownSubTypes) {
        if (s == st) {
            return true;
        }
    }
    return false;
}

// Map a raw firmware channel_error token to a user-facing message. The firmware
// emits machine tokens (e.g. "no_filament") that are meaningless to a user and
// were previously shown verbatim in the AMS loading-error modal. Unknown tokens
// fall back to the raw string so we never hide a novel error behind a generic
// message. Single source of truth shared by the error-set path here and any
// modal that surfaces operation_detail.
[[nodiscard]] std::string friendly_channel_error(const std::string& token, int lane_index) {
    if (token == "no_filament") {
        return fmt::format("No filament in lane {}. Load filament and retry.", lane_index + 1);
    }
    return token;
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

AmsBackendSnapmaker::AmsBackendSnapmaker(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info
    system_info_.type = AmsType::SNAPMAKER;
    system_info_.type_name = "Snapmaker SnapSwap";
    system_info_.supports_endless_spool = false;
    system_info_.supports_tool_mapping = false;
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;
    // The U1 has no filament cutter and forms no discrete tip — unload is just
    // heat + retract. Leaving tip_method at its CUT default mislabels the unload
    // stepper's middle phase "Cut & retract"; NONE drives the 2-step
    // "Heat nozzle -> Retract" stepper (see recreate_step_progress_for_operation).
    system_info_.tip_method = TipMethod::NONE;

    // Initialize 1 unit with 4 slots
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "SnapSwap";
    unit.display_name = "SnapSwap";
    unit.slot_count = NUM_TOOLS;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.topology = PathTopology::PARALLEL;

    for (int i = 0; i < NUM_TOOLS; i++) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        slot.extruder_name = (i == 0) ? "extruder" : fmt::format("extruder{}", i);
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    system_info_.total_slots = NUM_TOOLS;

    // Snapmaker U1 has a fixed 1:1 tool↔slot mapping (4 extruders, 4 slots).
    // Without this, ui_gcode_viewer_apply_ams_tool_colors() short-circuits on
    // an empty map and the 2D toolpath renders in whatever single color the
    // slicer wrote into filament_palette[initial_tool_index] — black on prints
    // where the initial tool's filament is dark.
    system_info_.tool_to_slot_map.reserve(NUM_TOOLS);
    for (int i = 0; i < NUM_TOOLS; i++) {
        system_info_.tool_to_slot_map.push_back(i);
    }

    spdlog::debug("[AMS Snapmaker] Backend created with {} tools", NUM_TOOLS);
}

// ============================================================================
// Lifecycle
// ============================================================================

void AmsBackendSnapmaker::on_started() {
    // Load persisted per-slot overrides (brand, spool name, spoolman IDs, etc.)
    // from the Moonraker DB lane_data namespace BEFORE any status parse runs.
    // AmsSubscriptionBackend::start() registers the WebSocket subscription
    // before on_started(); a status notification could in principle fire on
    // the libhv thread while we're still inside load_blocking(). Holding
    // mutex_ only during the swap keeps the parse path's read of overrides_
    // coherent without blocking it during the 5s DB round-trip.
    if (!api_)
        return;

    override_store_ = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api_, "snapmaker");
    auto loaded = override_store_->load_blocking();
    const auto loaded_count = loaded.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_ = std::move(loaded);
    }
    spdlog::info("{} Loaded {} slot overrides from filament_slot store", backend_log_tag(),
                 loaded_count);
}

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendSnapmaker::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendSnapmaker::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    SlotInfo empty;
    empty.slot_index = -1;
    return empty;
}

// ============================================================================
// Path Visualization
// ============================================================================

PathSegment AmsBackendSnapmaker::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE;
    }
    return PathSegment::SPOOL;
}

PathSegment AmsBackendSnapmaker::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot)
        return PathSegment::NONE;

    // Toolhead motion sensor empty for this tool. That alone does NOT mean
    // "no filament" — on the U1 an unload retracts filament out of the toolhead
    // but leaves it staged in the bowden/buffer (channel_state preload_finish),
    // where the buffer-side port sensor still reads present. Distinguish the
    // two cases with the port sensor (hardware capture 2026-06-13, tool T2:
    // e2_filament motion=false, filament_feed right.extruder2 detected=true):
    //   - port present  → filament threaded through the bowden but short of the
    //                      toolhead sensor: draw the line down to the toolhead
    //                      entry sensor dot but no farther (OUTPUT). The dot
    //                      stays hollow because the toolhead sensor reads empty.
    //   - port empty too → genuine runout / no filament: draw nothing (NONE).
    if (slot_index >= 0 && slot_index < NUM_TOOLS && !sensor_filament_present_[slot_index]) {
        if (port_sensor_filament_present_[slot_index])
            return PathSegment::OUTPUT;
        return PathSegment::NONE;
    }

    // PARALLEL multi-toolhead machine: each tool feeds its own dedicated
    // nozzle. Reaching here means this tool's motion sensor reads filament
    // present (the runout early-return above didn't fire), so filament is
    // threaded all the way into THIS tool's toolhead — render NOZZLE.
    //
    // The firmware marks only the *active* tool LOADED; every other
    // physically-loaded toolhead reports AVAILABLE. Previously AVAILABLE
    // (when not the active slot) rendered HUB, so the line stopped short at
    // the sensor dot for loaded-but-parked tools. On this topology a present
    // tool always has filament at its own nozzle, so both LOADED and
    // AVAILABLE render NOZZLE.
    if (slot->status == SlotStatus::LOADED || slot->status == SlotStatus::AVAILABLE) {
        return PathSegment::NOZZLE;
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendSnapmaker::infer_error_segment() const {
    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendSnapmaker::load_filament(int slot_index) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    // Snapmaker U1 firmware: AUTO_FEEDING is a thin macro wrapper that
    // forwards to the underlying FEED_AUTO command with module/channel
    // resolved from _FILAMENT_FEED_VARIABLE. FEED_AUTO has explicit LOAD /
    // UNLOAD / AUTO parameters — passing none of them is a silent no-op
    // (cmd_FEED_AUTO falls through every branch and returns).
    //
    // We must pass LOAD=1 to actually trigger the feed sequence. PRINTING
    // is left at the default 0 so we skip the firmware's port-input
    // filament-detected gate (which silent-returns when PRINTING=1 and the
    // port sensor reads no filament — the exact runout-recovery state).
    //
    // Trail of bad guesses, in order:
    //   1. T{n} — no-op when target tool already active (Klipper logged
    //      "Extruder extruderN already active"). That's always the case
    //      after a runout, so loads did nothing.
    //   2. AUTO_FEEDING EXTRUDER={n} — silent no-op because no LOAD
    //      parameter was passed; cmd_FEED_AUTO fell through.
    //   3. SM_PRINT_AUTO_FEED — gated on print_task_config.extruders_used,
    //      which can be all-false on a partially-extruded paused print.
    // The firmware's cmd_FEED_AUTO is the definitive reference; see
    // /home/lava/klipper/klippy/extras/filament_feed.py around line 1681.
    return execute_gcode(fmt::format("AUTO_FEEDING EXTRUDER={} LOAD=1", slot_index));
}

AmsError AmsBackendSnapmaker::unload_filament(int slot_index) {
    // Unload must mirror load: route through AUTO_FEEDING (the firmware macro
    // that forwards to FEED_AUTO with module/channel resolved from
    // _FILAMENT_FEED_VARIABLE), passing UNLOAD=1. FEED_AUTO with no STAGE runs
    // the full unload state machine (prepare → home → pick → heat → unload →
    // finish), which drives the firmware's per-channel feed state all the way
    // to "unload_finish".
    //
    // The bare INNER_FILAMENT_UNLOAD macro is the *leaf* the state machine
    // invokes internally — calling it directly skips the state transitions.
    // On a plain U1 that's fine, but it breaks aftermarket feeders that hook
    // the unload-finish state: the DnG-Crafts U1-Ace mod retracts the Anycubic
    // ACE Pro spool only when a channel reaches "unload_finish", so the bare
    // macro left filament dangling at the toolhead (prestonbrown/helixscreen#974).
    //
    // Resolve which extruder to unload: callers usually pass nothing (-1,
    // "the currently loaded one"), so fall back to current_slot. If we still
    // don't know which slot is loaded, fall back to the firmware's bare unload
    // rather than guess an EXTRUDER index.
    int extruder = slot_index;
    if (extruder < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        extruder = system_info_.current_slot;
    }
    if (extruder < 0) {
        return execute_gcode("INNER_FILAMENT_UNLOAD");
    }

    auto err = validate_slot_index(extruder);
    if (err.result != AmsResult::SUCCESS)
        return err;

    return execute_gcode(fmt::format("AUTO_FEEDING EXTRUDER={} UNLOAD=1", extruder));
}

bool AmsBackendSnapmaker::can_unload_from_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot || !slot->is_present()) {
        return false;
    }
    // Filament must be at THIS toolhead, not merely parked in the buffer. The
    // per-tool motion sensor (e{N}_filament) reads true only while filament is
    // loaded to the toolhead; after an unload it retracts to the buffer and the
    // sensor drops to false even though filament_exist (and thus is_present())
    // stays true. Without this the menu kept offering Unload for an
    // already-unloaded tool. See the header note + 2026-06-12 hardware capture.
    return sensor_filament_present_[slot_index];
}

bool AmsBackendSnapmaker::slot_has_filament_at_toolhead(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    return sensor_filament_present_[slot_index];
}

bool AmsBackendSnapmaker::slot_is_actively_loaded(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return false;
    }
    const auto* slot = system_info_.get_slot_global(slot_index);
    return slot && slot->status == SlotStatus::LOADED;
}

AmsError AmsBackendSnapmaker::select_slot(int slot_index) {
    return change_tool(slot_index);
}

AmsError AmsBackendSnapmaker::change_tool(int tool_number) {
    auto err = validate_slot_index(tool_number);
    if (err.result != AmsResult::SUCCESS)
        return err;

    return execute_gcode(fmt::format("T{}", tool_number));
}

// ============================================================================
// Recovery (not supported)
// ============================================================================

AmsError AmsBackendSnapmaker::recover() {
    return AmsErrorHelper::not_supported("Recover not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::reset() {
    return AmsErrorHelper::not_supported("Reset not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::cancel() {
    return AmsErrorHelper::not_supported("Cancel not supported on Snapmaker");
}

// ============================================================================
// Resume Preparation
// ============================================================================

// NOTE: This method has no caller in tree right now. The auto-recover-on-pause
// path that consumed it was pulled after field testing showed the
// motion=false + port=true signal can't distinguish "stale encoder, filament
// at gear" from "filament preloaded 4-inches short of gear" (firmware assist
// motor stops at preload_finish). Kept as detection infrastructure for the
// deferred follow-up in task #19 — when we have a verifiable signal that
// filament is *at* the extruder gear (likely filament_feed.channel_state ==
// 'load_finish'), this gate logic gets revived and tightened.
bool AmsBackendSnapmaker::is_stuck_motion_sensor_runout(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int slot = slot_index;
    if (slot < 0) {
        // Snapmaker has 1:1 tool↔slot mapping — current_tool and current_slot
        // hold the same value. Either is correct; current_slot matches the
        // convention used by other AmsBackend methods that take slot_index.
        slot = system_info_.current_slot;
    }
    if (slot < 0 || slot >= NUM_TOOLS) {
        return false;
    }
    // Motion (encoder) sensor latched false, port (buffer) sensor still
    // sees filament. The encoder is stale — the slot has physical filament
    // ready to feed, the motion sensor just hasn't been re-armed by extrusion.
    return !sensor_filament_present_[slot] && port_sensor_filament_present_[slot];
}

AmsBackendSnapmaker::~AmsBackendSnapmaker() = default;

void AmsBackendSnapmaker::prepare_for_resume(int slot_index, ResumeReadyCallback on_ready) {
    // Resolve target slot. Caller passes -1 when they don't know which tool
    // is active — fall back to system_info_.current_tool. If still unset, no
    // active tool means there's nothing to prep; just unblock the caller.
    int slot = slot_index;
    bool sensor_present = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot < 0) {
            // Snapmaker has 1:1 tool↔slot mapping — current_tool and
            // current_slot hold the same value. current_slot matches the
            // slot_index convention used by other AmsBackend methods.
            slot = system_info_.current_slot;
        }
        if (slot >= 0 && slot < NUM_TOOLS) {
            sensor_present = sensor_filament_present_[slot];
        }
    }

    if (slot < 0 || slot >= NUM_TOOLS) {
        spdlog::debug("{} prepare_for_resume: no active tool, skipping prep", backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
        return;
    }

    // Classify why the print paused. Terminal causes (confirmed under #991)
    // get the restart modal up front; everything else attempts RESUME
    // (default-recoverable). A RESUME that truly no-ops returns an ERROR through
    // the RESUME gcode callback, which the dispatch layer handles. Replaces the
    // old blunt virtual_sdcard gate.
    helix::PauseSignals sig;
    sig.exception_id = get_printer_state().get_print_exception_id();
    // On these firmware pauses print_stats.message is empty — the reason text
    // lives in exception.message. Fall back to print_stats.message when the
    // exception carries no text (e.g. non-Snapmaker pause paths).
    sig.message = get_printer_state().get_print_exception_message();
    if (sig.message.empty()) {
        sig.message = lv_subject_get_string(get_printer_state().get_print_message_subject());
    }
    sig.sdcard_active = get_printer_state().is_sdcard_active();
    sig.runout_tripped = !sensor_present;
    if (helix::classify_pause(sig, helix::snapmaker_terminal_matchers()) ==
        helix::PauseCause::Terminal) {
        spdlog::warn("{} prepare_for_resume: classified Terminal — surfacing restart UX",
                     backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::resume_requires_restart("classify_pause: Terminal"));
        }
        return;
    }

    if (sensor_present) {
        // Klipper's motion sensor reads filament — RESUME can clear its own
        // exception once extrusion resumes. No backend prep needed.
        spdlog::info("{} prepare_for_resume: tool {} sensor reports filament present, "
                     "skipping recovery",
                     backend_log_tag(), slot);
        if (on_ready) {
            on_ready(AmsErrorHelper::success());
        }
        return;
    }

    // Sensor reads runout. The firmware's own INNER_RESUME auto-feed
    // (SM_PRINT_AUTO_FEED) is gated on print_task_config.extruders_used[slot],
    // which the firmware freezes False for the duration of a print — so a plain
    // RESUME no-ops the feed and immediately re-pauses at CHECK_FILAMENT_RUNOUT.
    // AUTO_FEEDING calls FEED_AUTO directly (no extruders_used gate): it homes,
    // switches to the tool, feeds filament from the AMS port across the gap to
    // the toolhead sensor, heats to the slot's filament temp, then extrudes and
    // flushes — leaving the channel at load_finish with the runout sensor reading
    // present, so the subsequent RESUME's CHECK_FILAMENT_RUNOUT passes. The gcode
    // blocks until load_finish (success) or raises (error), and is idempotent
    // (FEED_AUTO returns early if already loaded). Verified live on a physical U1
    // (#991); replaces the old chain whose SET_FILAMENT_SENSOR ENABLE=0 silently
    // neutered FEED_AUTO and whose SET_PRINT_FILAMENT_CONFIG / manual extrude were
    // both unnecessary (INNER_RESUME restores config; AUTO_FEEDING does the heat).
    if (!api_) {
        spdlog::warn("{} prepare_for_resume: MoonrakerAPI unavailable", backend_log_tag());
        if (on_ready) {
            on_ready(AmsErrorHelper::not_connected("MoonrakerAPI unavailable"));
        }
        return;
    }

    std::string chain = fmt::format("AUTO_FEEDING EXTRUDER={0} LOAD=1 PRINTING=1", slot);
    spdlog::info("{} prepare_for_resume: tool {} runout latched — driving AMS load "
                 "(AUTO_FEEDING) before RESUME",
                 backend_log_tag(), slot);

    auto tok = lifetime_.token();
    const char* tag = backend_log_tag();
    MoonrakerAPI* api_ptr = api_;
    api_ptr->execute_gcode(
        chain,
        [this, tok, on_ready, tag, slot]() mutable {
            // MoonrakerAPI callbacks fire on the libhv WebSocket thread; defer to
            // main so on_ready (and the backstop arm) run on the UI thread.
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.ok",
                      [this, cb = std::move(on_ready), tag, slot]() {
                          spdlog::info("{} prepare_for_resume: tool {} AMS load complete "
                                       "(load_finish)",
                                       tag, slot);
                          // Hand control back so the caller dispatches RESUME.
                          if (cb) {
                              cb(AmsErrorHelper::success());
                          }
                      });
        },
        [this, tok, on_ready, tag, slot](const MoonrakerError& err) mutable {
            std::string msg = err.message;
            tok.defer("AmsBackendSnapmaker::prepare_for_resume.err",
                      [this, cb = std::move(on_ready), tag, slot, msg]() {
                          spdlog::error("{} prepare_for_resume: tool {} AMS load failed: {}",
                                        tag, slot, msg);
                          // Load failed → RESUME is never dispatched; report failure.
                          if (cb) {
                              cb(AmsError(AmsResult::COMMAND_FAILED,
                                          "prepare_for_resume AMS load failed: " + msg,
                                          "Filament reload before resume failed"));
                          }
                      });
        },
        // AUTO_FEEDING heats from cold + feeds + flushes; measured ~86s live, so
        // give generous headroom.
        /*timeout_ms=*/150000,
        /*silent=*/true);
}

// ============================================================================
// Configuration
// ============================================================================

AmsError AmsBackendSnapmaker::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS)
        return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return AmsErrorHelper::invalid_slot(slot_index, NUM_TOOLS - 1);

        // Update the in-memory slot directly. Covers every SlotInfo field the
        // caller may set — a persist=false preview must not silently drop
        // brand / spool_name / spoolman_* / weights / color_name because
        // otherwise the UI would snap back on the next get_slot_info read.
        slot->color_name = info.color_name;
        slot->color_rgb = info.color_rgb;
        slot->material = info.material;
        slot->brand = info.brand;
        slot->nozzle_temp_min = info.nozzle_temp_min;
        slot->nozzle_temp_max = info.nozzle_temp_max;
        slot->bed_temp = info.bed_temp;
        slot->remaining_weight_g = info.remaining_weight_g;
        slot->total_weight_g = info.total_weight_g;
        slot->spoolman_id = info.spoolman_id;
        slot->spoolman_vendor_id = info.spoolman_vendor_id;
        slot->spool_name = info.spool_name;

        // Previously this function IGNORED the persist parameter — user edits
        // were in-memory only and the next Klipper status update wiped them
        // via handle_status_update's unconditional writes from RFID and
        // print_task_config. For persist=true, stage the override into
        // overrides_ now so apply_overrides layers it back over firmware data
        // on every subsequent parse. For persist=false we explicitly do NOT
        // touch overrides_ — preview edits are in-memory only and will be
        // overwritten by the next firmware parse, which is the expected
        // preview contract.
        //
        // NOTE on self-wipe: the AD5X IFS implementation pre-updates
        // last_firmware_color_ here to prevent the color-based hardware-event
        // check from misreading a user color edit as a physical spool swap.
        // Snapmaker's hardware-event check is RFID-UID-based, and the user
        // cannot set a CARD_UID through the edit UI — SlotInfo has no UID
        // field. So last_rfid_uid_ stays at whatever the firmware last
        // reported, and the next parse compares firmware UID against that
        // baseline exactly as intended. No pre-update needed here.
        if (persist) {
            helix::ams::FilamentSlotOverride ovr;
            ovr.brand = info.brand;
            ovr.spool_name = info.spool_name;
            ovr.spoolman_id = info.spoolman_id;
            ovr.spoolman_vendor_id = info.spoolman_vendor_id;
            ovr.remaining_weight_g = info.remaining_weight_g;
            ovr.total_weight_g = info.total_weight_g;
            ovr.color_rgb = info.color_rgb;
            ovr.color_set = true; // a user-edit always records a color, even pure black (#000000)
            ovr.color_name = info.color_name;
            ovr.material = info.material;
            // User-lock: persist=true edits are sticky against the
            // OverwriteAlways auto-mirror (#965). Material is only locked
            // when the user provided one; an explicit empty material means
            // "let bootstrap fill it on next firmware report."
            ovr.user_locked_color = true;
            ovr.user_locked_material = !info.material.empty();
            // SlotInfo carries the user's edit OR the bound Spoolman spool's
            // filament profile; the material-DB fallback for fields left at 0
            // is applied at emit time inside resolved_temps(). Centralized in
            // the helper so the four AMS backends stay in sync.
            helix::ams::populate_temps_from_slot_info(ovr, info);
            // updated_at left default — save_async stamps a fresh value.
            overrides_[slot_index] = ovr;
        }
    }

    if (persist && override_store_) {
        // Re-read from overrides_ under the lock to get the staged copy.
        helix::ams::FilamentSlotOverride ovr_to_save;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = overrides_.find(slot_index);
            if (it != overrides_.end()) {
                ovr_to_save = it->second;
            }
        }
        // Capture backend_log_tag by value — save_async's MR callback may fire
        // long after this returns (MR tracker ~60s timeout). Do NOT capture
        // `this`: the backend may outlive its store, but the store will
        // outlive the scheduled save by design.
        const std::string tag = backend_log_tag();
        override_store_->save_async(
            slot_index, ovr_to_save, [tag, slot_index](bool success, const std::string& err) {
                if (!success) {
                    spdlog::warn("{} Override persist failed for slot {}: {}", tag, slot_index,
                                 err);
                }
            });
    }

    // Push the user's edit back to firmware via the paxx12 Extended Firmware
    // POST /printer/filament_detect/set endpoint (see filament_detect.md in
    // SnapmakerU1-Extended-Firmware/docs/design/). Firmware mirrors the body
    // into print_task_config.filament_vendor / filament_type / filament_color_rgba,
    // which the parse path here already reads — so on the next status update
    // OverwriteAlways auto-mirror sees firmware-truth that matches user-truth
    // and lane_data converges. On stock firmware (no extension) the endpoint
    // 404s; the override is still persisted to lane_data, so HelixScreen's UI
    // works correctly. Only OrcaSlicer's MoonrakerPrinterAgent and the
    // firmware-side LCD don't reflect user edits on stock firmware.
    if (persist && api_) {
        nlohmann::json info_obj = nlohmann::json::object();
        if (!info.brand.empty())
            info_obj["VENDOR"] = info.brand;
        if (!info.material.empty())
            info_obj["MAIN_TYPE"] = info.material;
        // SUB_TYPE is restricted to Snapmaker's known product lines per the
        // firmware spec. spool_name carries the SUB_TYPE on the read path
        // (see handle_status_update), but UI-edited spool_name may be a free-
        // form string ("My Custom Spool"). Only round-trip when it matches a
        // known sub_type — otherwise omit and let firmware preserve whatever
        // it had. The free-form string still lives in lane_data. Shares the
        // is_known_subtype() helper with the #991 resume re-assert path.
        if (is_known_subtype(info.spool_name)) {
            info_obj["SUB_TYPE"] = info.spool_name;
        }
        info_obj["RGB_1"] = info.color_rgb;
        info_obj["ALPHA"] = 255;
        if (info.nozzle_temp_min > 0)
            info_obj["HOTEND_MIN_TEMP"] = info.nozzle_temp_min;
        if (info.nozzle_temp_max > 0)
            info_obj["HOTEND_MAX_TEMP"] = info.nozzle_temp_max;
        if (info.bed_temp > 0)
            info_obj["BED_TEMP"] = info.bed_temp;
        // CARD_UID and SKU intentionally omitted — SlotInfo doesn't carry
        // them and we want firmware to preserve whatever the RFID tag wrote.

        nlohmann::json payload = nlohmann::json::object();
        payload["channel"] = slot_index;
        payload["info"] = info_obj;

        // Log-only callback — no UI / member access — so a value-captured tag
        // is safe even after the backend is destroyed (same rationale as
        // save_async's callback above). Routes through MoonrakerRestAPI which
        // dispatches on its own HTTP worker thread, NOT a raw std::thread
        // (lesson L083: pthread EAGAIN on AD5M / CC1 / MIPS32).
        const std::string tag = backend_log_tag();
        api_->rest().call_rest_post(
            "/printer/filament_detect/set", payload, [tag, slot_index](const RestResponse& resp) {
                if (!resp.success) {
                    // 404 on stock firmware (no Extended Firmware extension)
                    // is expected — log at debug, not warn, so we don't spam
                    // every user without the firmware update.
                    if (resp.status_code == 404) {
                        spdlog::debug("{} filament_detect/set unavailable (slot {}): "
                                      "stock firmware without Extended Firmware extension",
                                      tag, slot_index);
                    } else {
                        spdlog::warn("{} filament_detect/set failed for slot {}: HTTP {} {}", tag,
                                     slot_index, resp.status_code, resp.error);
                    }
                    return;
                }
                // Success-shaped HTTP response can still carry "state":"error"
                // (per filament_detect.md). Drain that as a warn — override is
                // still saved to lane_data so user data isn't lost.
                if (resp.data.is_object()) {
                    auto state_it = resp.data.find("state");
                    if (state_it != resp.data.end() && state_it->is_string() &&
                        state_it->get<std::string>() == "error") {
                        std::string msg;
                        auto msg_it = resp.data.find("message");
                        if (msg_it != resp.data.end() && msg_it->is_string()) {
                            msg = msg_it->get<std::string>();
                        }
                        spdlog::warn("{} filament_detect/set returned error for slot {}: {}", tag,
                                     slot_index, msg);
                    }
                }
            });
    }

    // Pass slot_index as event data so AmsState can do a targeted slot sync.
    // Without it, AmsState::on_event silently skips the refresh and the AMS
    // panel never re-reads the edited slot — the UI shows stale data until
    // the next firmware status notification triggers a full refresh.
    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendSnapmaker::set_tool_mapping(int /*tool_number*/, int /*slot_index*/) {
    return AmsErrorHelper::not_supported("Tool mapping not supported on Snapmaker");
}

// ============================================================================
// Bypass (not applicable)
// ============================================================================

AmsError AmsBackendSnapmaker::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

// ============================================================================
// Static Parsers
// ============================================================================

ExtruderToolState AmsBackendSnapmaker::parse_extruder_state(const nlohmann::json& json) {
    ExtruderToolState state;

    if (json.contains("state") && json["state"].is_string()) {
        state.state = json["state"].get<std::string>();
    }
    if (json.contains("park_pin") && json["park_pin"].is_boolean()) {
        state.park_pin = json["park_pin"].get<bool>();
    }
    if (json.contains("active_pin") && json["active_pin"].is_boolean()) {
        state.active_pin = json["active_pin"].get<bool>();
    }
    if (json.contains("activating_move") && json["activating_move"].is_boolean()) {
        state.activating_move = json["activating_move"].get<bool>();
    }
    if (json.contains("extruder_offset") && json["extruder_offset"].is_array()) {
        const auto& arr = json["extruder_offset"];
        for (size_t i = 0; i < std::min(arr.size(), size_t{3}); i++) {
            if (arr[i].is_number()) {
                state.extruder_offset[i] = arr[i].get<float>();
            }
        }
    }
    if (json.contains("switch_count") && json["switch_count"].is_number()) {
        state.switch_count = json["switch_count"].get<int>();
    }
    if (json.contains("retry_count") && json["retry_count"].is_number()) {
        state.retry_count = json["retry_count"].get<int>();
    }
    if (json.contains("error_count") && json["error_count"].is_number()) {
        state.error_count = json["error_count"].get<int>();
    }

    return state;
}

SnapmakerRfidInfo AmsBackendSnapmaker::parse_rfid_info(const nlohmann::json& json) {
    SnapmakerRfidInfo info;

    if (json.contains("MAIN_TYPE") && json["MAIN_TYPE"].is_string()) {
        info.main_type = json["MAIN_TYPE"].get<std::string>();
    }
    if (json.contains("SUB_TYPE") && json["SUB_TYPE"].is_string()) {
        info.sub_type = json["SUB_TYPE"].get<std::string>();
    }
    if (json.contains("MANUFACTURER") && json["MANUFACTURER"].is_string()) {
        info.manufacturer = json["MANUFACTURER"].get<std::string>();
    }
    if (json.contains("VENDOR") && json["VENDOR"].is_string()) {
        info.vendor = json["VENDOR"].get<std::string>();
    }
    if (json.contains("ARGB_COLOR") && json["ARGB_COLOR"].is_number()) {
        // ARGB -> RGB: mask off the alpha byte
        uint32_t argb = json["ARGB_COLOR"].get<uint32_t>();
        info.color_rgb = argb & 0x00FFFFFF;
    }
    if (json.contains("HOTEND_MIN_TEMP") && json["HOTEND_MIN_TEMP"].is_number()) {
        info.hotend_min_temp = json["HOTEND_MIN_TEMP"].get<int>();
    }
    if (json.contains("HOTEND_MAX_TEMP") && json["HOTEND_MAX_TEMP"].is_number()) {
        info.hotend_max_temp = json["HOTEND_MAX_TEMP"].get<int>();
    }
    if (json.contains("BED_TEMP") && json["BED_TEMP"].is_number()) {
        info.bed_temp = json["BED_TEMP"].get<int>();
    }
    if (json.contains("WEIGHT") && json["WEIGHT"].is_number()) {
        info.weight_g = json["WEIGHT"].get<int>();
    }
    // CARD_UID is a 4-byte array like [144, 32, 196, 2]. Canonicalize to a
    // comma-joined string so the override system's baseline comparison is a
    // simple string == string check. Empty / missing array stays as empty
    // string (treated as "no tag / unread" by check_hardware_event_clear).
    if (json.contains("CARD_UID") && json["CARD_UID"].is_array()) {
        const auto& arr = json["CARD_UID"];
        std::string uid;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                // If any byte isn't a number, bail out — partial UIDs aren't
                // safe to compare. Leave info.uid empty so the check is a
                // no-op for this parse.
                uid.clear();
                break;
            }
            if (!uid.empty())
                uid.push_back(',');
            uid += std::to_string(arr[i].get<int>());
        }
        info.uid = std::move(uid);
    }

    return info;
}

// ============================================================================
// Status Update Handling
// ============================================================================

void AmsBackendSnapmaker::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update format: {"method":"notify_status_update","params":[{...}, timestamp]}
    // Initial query responses send unwrapped status directly — handle both.
    const nlohmann::json* status_ptr = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status_ptr = &notification["params"][0];
    }
    const auto& status = *status_ptr;
    if (!status.is_object())
        return;

    bool changed = false;
    // Set when the active-tool port-present flag changed this parse (#991), so
    // we publish to AmsState exactly once after releasing the mutex.
    bool port_present_changed = false;

    // Per-slot UID observed THIS parse. Empty string means no RFID info in
    // this notification (incremental update, or slot not included). Only
    // populated when filament_detect.info is present and parse_rfid_info
    // returns a non-empty UID. check_hardware_event_clear then sees the
    // observed UID (or empty = no signal) and updates / clears accordingly.
    std::array<std::string, NUM_TOOLS> observed_uids;
    std::array<bool, NUM_TOOLS> saw_rfid_info{};

    { // Scope lock — emit_event MUST be called outside mutex_ to avoid deadlock
      // with sync_from_backend() which acquires mutex_ via get_system_info()
        std::lock_guard<std::mutex> lock(mutex_);

        // Parse extruder0..3 state
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        static const std::string extruder_keys[] = {"extruder", "extruder1", "extruder2",
                                                    "extruder3"};
        for (int i = 0; i < NUM_TOOLS; i++) {
            const auto& key = extruder_keys[i];
            if (status.contains(key) && status[key].is_object()) {
                auto new_state = parse_extruder_state(status[key]);

                // Update slot status based on extruder state (only if pin state changed)
                auto* slot = system_info_.units[0].get_slot(i);
                if (slot) {
                    SlotStatus prev = slot->status;
                    if (new_state.active_pin) {
                        slot->status = SlotStatus::LOADED;
                    } else if (new_state.park_pin) {
                        slot->status = SlotStatus::AVAILABLE;
                    }
                    if (slot->status != prev)
                        changed = true;
                }

                extruder_states_[i] = std::move(new_state);
            }
        }

        // Detect active tool from extruder pin state and toolhead.extruder.
        // Only update when we have actual evidence — incremental status updates
        // may omit extruder/toolhead keys, so preserve the current value when
        // no relevant data is present (prevents oscillation between valid and -1).
        bool has_extruder_data = false;
        int active = -1;
        for (int i = 0; i < NUM_TOOLS; i++) {
            if (extruder_states_[i].active_pin ||
                (!extruder_states_[i].state.empty() && extruder_states_[i].state != "PARKED")) {
                active = i;
                has_extruder_data = true;
                break;
            }
        }
        if (status.contains("toolhead") && status["toolhead"].is_object()) {
            const auto& th = status["toolhead"];
            if (th.contains("extruder") && th["extruder"].is_string()) {
                auto ext_name = th["extruder"].get<std::string>();
                // "extruder" = 0, "extruder1" = 1, etc.
                if (ext_name == "extruder") {
                    active = 0;
                } else if (ext_name.size() > 8 && ext_name.rfind("extruder", 0) == 0) {
                    try {
                        active = std::stoi(ext_name.substr(8));
                    } catch (...) {
                    }
                }
                has_extruder_data = true;
            }
        }
        if (has_extruder_data && active != system_info_.current_tool) {
            // Demote previous active tool from LOADED to AVAILABLE
            if (system_info_.current_tool >= 0 && system_info_.current_tool < NUM_TOOLS) {
                auto* prev_slot = system_info_.units[0].get_slot(system_info_.current_tool);
                if (prev_slot && prev_slot->status == SlotStatus::LOADED) {
                    prev_slot->status = SlotStatus::AVAILABLE;
                }
            }
            system_info_.current_tool = active;
            system_info_.current_slot = active; // 1:1 tool-to-slot on Snapmaker
            system_info_.filament_loaded = (active >= 0);
            // Mark active tool's slot as LOADED
            if (active >= 0 && active < NUM_TOOLS) {
                auto* slot = system_info_.units[0].get_slot(active);
                if (slot && slot->status != SlotStatus::EMPTY) {
                    slot->status = SlotStatus::LOADED;
                }
            }
            changed = true;
        }

        // Parse filament_detect info (RFID data per channel)
        if (status.contains("filament_detect") && status["filament_detect"].is_object()) {
            const auto& fd = status["filament_detect"];

            // Parse RFID info per channel — filament_detect.info is a JSON array [ch0, ch1, ch2,
            // ch3] Only apply RFID data when it contains real values (not "NONE").
            // print_task_config is the authoritative source; RFID supplements it when tags are
            // present.
            if (fd.contains("info") && fd["info"].is_array()) {
                const auto& info_arr = fd["info"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(info_arr.size()); i++) {
                    if (!info_arr[i].is_object())
                        continue;
                    auto rfid = parse_rfid_info(info_arr[i]);

                    // Capture the UID for hardware-swap detection before any early
                    // exit. Even "NONE" tags can carry a CARD_UID in theory, and
                    // we want the observed value visible to check_hardware_event_clear
                    // regardless of whether we apply the rest of the RFID fields.
                    observed_uids[i] = rfid.uid;
                    saw_rfid_info[i] = true;

                    // Skip entirely if RFID reader is disabled or no tag present
                    if (rfid.main_type == "NONE")
                        continue;

                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        slot->material = rfid.main_type;
                        auto brand = !rfid.manufacturer.empty() ? rfid.manufacturer : rfid.vendor;
                        if (brand != "NONE")
                            slot->brand = brand;
                        slot->color_rgb = rfid.color_rgb;
                        // SUB_TYPE is Snapmaker's filament product-line name (e.g.
                        // "SnapSpeed" for their PLA line — akin to Polymaker's
                        // "PolyLite"). Maps to spool_name, NOT color_name. The
                        // Snapmaker RFID doesn't expose a dedicated color-name
                        // field — color_name stays unset here and is user-editable
                        // via the edit modal's color picker.
                        if (rfid.sub_type != "NONE")
                            slot->spool_name = rfid.sub_type;
                        slot->nozzle_temp_min = rfid.hotend_min_temp;
                        slot->nozzle_temp_max = rfid.hotend_max_temp;
                        slot->bed_temp = rfid.bed_temp;
                        slot->total_weight_g = static_cast<float>(rfid.weight_g);
                    }
                    changed = true;
                }
            }

            // Parse filament state per channel — filament_detect.state is [int, int, int, int]
            // 1 = filament present, 0 = no filament / no tag
            if (fd.contains("state") && fd["state"].is_array()) {
                const auto& state_arr = fd["state"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(state_arr.size()); i++) {
                    if (!state_arr[i].is_number())
                        continue;
                    int state_val = state_arr[i].get<int>();
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        // Only set from filament_detect if extruder state hasn't already
                        // provided a more authoritative status (LOADED/AVAILABLE via
                        // park_pin/active_pin)
                        if (slot->status == SlotStatus::UNKNOWN) {
                            slot->status =
                                (state_val != 0) ? SlotStatus::AVAILABLE : SlotStatus::EMPTY;
                        }
                        changed = true;
                    }
                }
            }
        }

        // Parse filament_feed left/right — top-level Klipper objects (not nested in
        // filament_detect) Each contains per-extruder state: filament_detected, channel_state,
        // channel_error
        for (const auto& feed_key : {"filament_feed left", "filament_feed right"}) {
            if (status.contains(feed_key) && status[feed_key].is_object()) {
                const auto& feed = status[feed_key];
                for (int i = 0; i < NUM_TOOLS; i++) {
                    std::string ext_key = (i == 0) ? "extruder0" : fmt::format("extruder{}", i);
                    if (feed.contains(ext_key) && feed[ext_key].is_object()) {
                        const auto& ch = feed[ext_key];
                        bool detected = ch.value("filament_detected", false);
                        // Mirror into port_sensor_filament_present_ so
                        // is_stuck_motion_sensor_runout can distinguish a real
                        // runout (both sensors false) from a stale motion-sensor
                        // false positive (motion=false, port=true). Tracked
                        // independent of slot->status because slot status flips
                        // to AVAILABLE/LOADED based on extruder pin state which
                        // is orthogonal to the port sensor reading.
                        if (i >= 0 && i < NUM_TOOLS) {
                            port_sensor_filament_present_[i] = detected;
                        }
                        auto* slot = system_info_.units[0].get_slot(i);
                        if (slot) {
                            if (detected && (slot->status == SlotStatus::EMPTY ||
                                             slot->status == SlotStatus::UNKNOWN)) {
                                slot->status = SlotStatus::AVAILABLE;
                                changed = true;
                            } else if (!detected && slot->status != SlotStatus::LOADED) {
                                slot->status = SlotStatus::EMPTY;
                                changed = true;
                            }
                        }

                        // Parse channel_state for load/unload action tracking
                        auto state = ch.value("channel_state", "");
                        auto error = ch.value("channel_error", "ok");
                        if (error != "ok" && !error.empty() && error != "none") {
                            // Distinguish a genuine load FAILURE from an idle,
                            // never-loaded EMPTY lane. The firmware reports
                            // channel_error="no_filament" for any lane sitting
                            // empty — including a lane deliberately left unloaded
                            // for a multi-color print (e.g. heads 0+2 used, head 1
                            // empty). Post-print that empty lane would latch the
                            // whole backend into action=Error="no_filament" and
                            // pop a spurious AMS loading-error modal. A real load
                            // failure happens during/after a load attempt
                            // (channel_state loading/preloading) or on a lane that
                            // actually has filament present, or on the active lane.
                            // Treat an error on an idle, empty, non-active lane as
                            // a non-event. (Snapmaker U1 false-alarm fix.)
                            const auto* slot = system_info_.units[0].get_slot(i);
                            const bool lane_empty =
                                slot == nullptr || !slot->is_present();
                            const bool load_in_progress =
                                state == "loading" || state == "preloading";
                            const bool active_lane = system_info_.current_slot == i ||
                                                     system_info_.current_tool == i;
                            if (lane_empty && !load_in_progress && !active_lane) {
                                spdlog::debug(
                                    "[AmsBackendSnapmaker] ignoring channel_error '{}' on idle "
                                    "empty lane {} (not loading, not active)",
                                    error, i);
                            } else {
                                system_info_.action = AmsAction::ERROR;
                                system_info_.operation_detail = friendly_channel_error(error, i);
                                changed = true;
                            }
                        } else if (state == "loading" || state == "preloading") {
                            system_info_.action = AmsAction::LOADING;
                            changed = true;
                        } else if (state == "unloading") {
                            system_info_.action = AmsAction::UNLOADING;
                            changed = true;
                        } else if (state == "unload_finish" || state == "preload_finish") {
                            // Unload completed: the firmware has retracted filament
                            // out of the toolhead into the buffer. The extruder
                            // pin-state path keeps filament_loaded=(active>=0) true
                            // because the tool's active_pin stays set while parked,
                            // so without this the active-lane highlight never clears
                            // after an idle unload (bottom T-badge stayed "active"
                            // while top-right correctly showed Idle). Clear the
                            // loaded state for THIS channel so the single
                            // active-loaded source of truth (slot_is_actively_loaded
                            // / filament_loaded) drops immediately.
                            auto* slot = system_info_.units[0].get_slot(i);
                            if (slot && slot->status == SlotStatus::LOADED) {
                                slot->status = SlotStatus::AVAILABLE;
                                changed = true;
                            }
                            if (system_info_.current_slot == i ||
                                system_info_.current_tool == i) {
                                system_info_.filament_loaded = false;
                                system_info_.current_slot = -1;
                                system_info_.current_tool = -1;
                                changed = true;
                            }
                            if (system_info_.action == AmsAction::LOADING ||
                                system_info_.action == AmsAction::UNLOADING) {
                                system_info_.action = AmsAction::IDLE;
                                system_info_.operation_detail.clear();
                                PostOpCooldownManager::instance().schedule();
                                changed = true;
                            }
                        } else if (state == "load_finish" || state == "idle") {
                            if (system_info_.action == AmsAction::LOADING ||
                                system_info_.action == AmsAction::UNLOADING) {
                                system_info_.action = AmsAction::IDLE;
                                system_info_.operation_detail.clear();
                                PostOpCooldownManager::instance().schedule();
                                changed = true;
                            }
                        }
                    }
                }
            }
        }

        // Parse print_task_config — authoritative filament info from Snapmaker's task manager
        // Contains per-extruder filament type, vendor, color, and presence data
        if (status.contains("print_task_config") && status["print_task_config"].is_object()) {
            const auto& ptc = status["print_task_config"];

            // filament_exist: [bool, bool, bool, bool] — whether filament is loaded per slot
            if (ptc.contains("filament_exist") && ptc["filament_exist"].is_array()) {
                const auto& exist_arr = ptc["filament_exist"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(exist_arr.size()); i++) {
                    if (!exist_arr[i].is_boolean())
                        continue;
                    bool exists = exist_arr[i].get<bool>();
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        if (exists && slot->status != SlotStatus::LOADED) {
                            slot->status = SlotStatus::AVAILABLE;
                        } else if (!exists) {
                            slot->status = SlotStatus::EMPTY;
                        }
                        changed = true;
                    }
                }
            }

            // filament_type: ["PLA", "PLA", ...] — material type per slot
            if (ptc.contains("filament_type") && ptc["filament_type"].is_array()) {
                const auto& type_arr = ptc["filament_type"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(type_arr.size()); i++) {
                    if (!type_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        auto type = type_arr[i].get<std::string>();
                        slot->material = type; // Base type only (e.g., "PLA") for compact display
                        changed = true;
                    }
                }
            }

            // filament_vendor: ["Snapmaker", ...] — brand per slot
            if (ptc.contains("filament_vendor") && ptc["filament_vendor"].is_array()) {
                const auto& vendor_arr = ptc["filament_vendor"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(vendor_arr.size()); i++) {
                    if (!vendor_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        slot->brand = vendor_arr[i].get<std::string>();
                        changed = true;
                    }
                }
            }

            // filament_color_rgba: ["080A0DFF", "E2DEDBFF", ...] — hex RGBA color per slot
            if (ptc.contains("filament_color_rgba") && ptc["filament_color_rgba"].is_array()) {
                const auto& color_arr = ptc["filament_color_rgba"];
                for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(color_arr.size()); i++) {
                    if (!color_arr[i].is_string())
                        continue;
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        auto hex = color_arr[i].get<std::string>();
                        // RGBA hex string → RGB uint32: take first 6 chars
                        if (hex.size() >= 6) {
                            try {
                                slot->color_rgb = std::stoul(hex.substr(0, 6), nullptr, 16);
                            } catch (...) {
                            }
                        }
                        changed = true;
                    }
                }
            }
        }

        // Parse filament_motion_sensor / filament_switch_sensor for per-slot
        // runout state. Snapmaker U1's config has [filament_motion_sensor e{N}_filament]
        // with pause_on_runout=True; when filament stops moving past the encoder,
        // Klipper publishes filament_detected:false and triggers PAUSE. The slot
        // status / extruder pin state don't reflect this — the tool is still
        // "active" but no filament reaches the nozzle. Mirror the sensor flag so
        // the path canvas can break the spool→toolhead line at runout.
        //
        // Match both prefixes (motion is the Snapmaker default; switch is the
        // generic fallback) and any "e{N}_filament" / "e{N}" sensor name suffix.
        for (auto it = status.begin(); it != status.end(); ++it) {
            const std::string& key = it.key();
            const auto motion_prefix = std::string_view("filament_motion_sensor ");
            const auto switch_prefix = std::string_view("filament_switch_sensor ");
            std::string_view sensor_name;
            if (key.compare(0, motion_prefix.size(), motion_prefix) == 0) {
                sensor_name = std::string_view(key).substr(motion_prefix.size());
            } else if (key.compare(0, switch_prefix.size(), switch_prefix) == 0) {
                sensor_name = std::string_view(key).substr(switch_prefix.size());
            } else {
                continue;
            }
            // Expect "e{N}_filament" or "e{N}". Anything else (toolhead_sensor,
            // bypass_sensor, custom names) is unrelated to per-tool runout.
            if (sensor_name.size() < 2 || sensor_name[0] != 'e')
                continue;
            int tool_idx = -1;
            try {
                size_t digit_end = 1;
                while (digit_end < sensor_name.size() &&
                       std::isdigit(static_cast<unsigned char>(sensor_name[digit_end]))) {
                    ++digit_end;
                }
                if (digit_end == 1)
                    continue; // no digits
                tool_idx = std::stoi(std::string(sensor_name.substr(1, digit_end - 1)));
            } catch (...) {
                continue;
            }
            if (tool_idx < 0 || tool_idx >= NUM_TOOLS)
                continue;
            if (!it.value().is_object())
                continue;
            // filament_detected: Klipper emits as bool; default true (no runout)
            // so missing field == "no change" via the contains check. Use .find()
            // + is_boolean() (per [L087]) rather than .value() which would throw
            // on a null payload.
            auto fd_it = it.value().find("filament_detected");
            if (fd_it == it.value().end() || !fd_it->is_boolean())
                continue;
            bool present = fd_it->get<bool>();
            if (sensor_filament_present_[tool_idx] != present) {
                sensor_filament_present_[tool_idx] = present;
                changed = true;
                spdlog::info("{} Tool {} filament sensor: {} ({})", backend_log_tag(), tool_idx,
                             present ? "PRESENT" : "RUNOUT", key);
            }
        }

        // If the active tool's filament sensor reports runout, the global
        // filament_loaded flag (used by get_filament_segment) should reflect that.
        // The pin-state path above sets filament_loaded=(active>=0) — override
        // here so the canvas's spool→toolhead line breaks on runout even though
        // the tool itself is still "active".
        if (system_info_.current_tool >= 0 && system_info_.current_tool < NUM_TOOLS &&
            !sensor_filament_present_[system_info_.current_tool]) {
            if (system_info_.filament_loaded) {
                system_info_.filament_loaded = false;
                changed = true;
            }
        }

        // Per-slot runout demotion: any slot whose motion sensor reports
        // no filament should be AVAILABLE (spool present, ready to feed), not
        // LOADED. Without this, the AMS context menu's Load button is gated off
        // (pending_is_loaded_ from slot.status==LOADED disables it) and the user
        // has no way to re-feed filament from the UI after a runout — they get
        // Unload/Reset on a slot that has no filament between feeder and nozzle.
        // EMPTY is wrong here because the slot's RFID/print_task_config still
        // reports a spool present; AVAILABLE accurately captures "spool yes,
        // filament-at-toolhead no".
        for (int i = 0; i < NUM_TOOLS; ++i) {
            if (sensor_filament_present_[i])
                continue;
            auto* slot = system_info_.units[0].get_slot(i);
            if (slot && slot->status == SlotStatus::LOADED) {
                slot->status = SlotStatus::AVAILABLE;
                changed = true;
            }
        }

        // Parse convergence point. After every firmware-sourced field on the
        // SlotInfo has been populated above, loop through slots and apply
        // user-configured overrides on top. check_hardware_event_clear must run
        // FIRST so it sees firmware-truth fields (not the override-masked view)
        // and can clear a stale override when a physical spool swap is detected.
        // apply_overrides runs after, so the final SlotInfo the UI reads through
        // get_slot_info / the emitted event reflects the override layer.
        //
        // Snapmaker has multiple parse paths feeding the same slot (RFID info,
        // print_task_config, filament_feed). Rather than hook the override logic
        // into each one, we run it once here at the tail — the tradeoff is that
        // get_slot_info during a partial parse would observe uncleared overrides,
        // but since everything runs under mutex_ and handle_status_update is the
        // only writer, there's no observable window.
        for (int i = 0; i < NUM_TOOLS; ++i) {
            auto* slot = system_info_.units[0].get_slot(i);
            if (!slot)
                continue;

            // Only pass a UID to the hardware-event check when this parse
            // actually carried filament_detect.info for the slot. Otherwise we'd
            // feed an empty-string UID on every incremental notify (e.g. pure
            // toolhead status updates) and defeat the "empty = no signal"
            // contract the helper expects. saw_rfid_info[i] captures "we had an
            // info blob"; observed_uids[i] may still be empty if the tag's
            // CARD_UID field was missing or malformed, which the helper also
            // treats as no signal.
            if (saw_rfid_info[i]) {
                check_hardware_event_clear(*slot, i, observed_uids[i]);
            }
            // Mirror firmware-truth color/material into lane_data so OrcaSlicer's
            // MoonrakerPrinterAgent sees the spool. OverwriteAlways policy: user
            // edits via set_slot_info now round-trip through firmware via the
            // POST /printer/filament_detect/set endpoint (paxx12 Extended Firmware),
            // so firmware-truth and user-truth converge — overwriting lane_data
            // unconditionally is safe and also catches external edits (CHANGE_ZCOLOR
            // from a print, manual gcode, OrcaSlicer, etc). On stock firmware the
            // POST 404s, but the override is still persisted to lane_data
            // separately, so this overwrite is the only path that could theoretically
            // de-sync — accept that tradeoff in exchange for picking up external
            // edits on extension-enabled firmware. See mirror_firmware_to_lane_data
            // docs and AD5X IFS for the same pattern.
            helix::ams::mirror_firmware_to_lane_data(
                override_store_.get(), overrides_, i, slot->color_rgb, slot->material,
                slot->status == SlotStatus::AVAILABLE, helix::ams::MirrorPolicy::OverwriteAlways,
                backend_log_tag());
            apply_overrides(*slot, i);
        }

        // First-gate (port) filament presence for the ACTIVE tool (#991). The
        // runout dialog gates Resume on THIS signal — the port/buffer sensor that
        // flips true the moment a user re-feeds a spool — NOT the toolhead motion
        // sensor (sensor_filament_present_), which stays "runout" until extrusion.
        // No active tool → treat as present (1) so Resume is never gated. Computed
        // under mutex_ (reads current_tool + the port array); published after the
        // mutex is released. Only publish on an actual change to avoid spamming
        // the UpdateQueue on every incremental notify.
        int active_tool = system_info_.current_tool;
        bool active_port_present =
            !(active_tool >= 0 && active_tool < NUM_TOOLS) ||
            port_sensor_filament_present_[active_tool];
        int port_val = active_port_present ? 1 : 0;
        if (port_val != last_published_port_present_) {
            last_published_port_present_ = port_val;
            port_present_changed = true;
        }

    } // Release mutex_ before emitting event

    if (port_present_changed) {
        AmsState::instance().set_active_tool_port_present(last_published_port_present_ != 0);
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// ============================================================================
// Override layering
// ============================================================================

void AmsBackendSnapmaker::apply_overrides(SlotInfo& slot, int slot_index) {
    // Every caller of apply_overrides runs under mutex_ (handle_status_update's
    // tail, set_slot_info's lock block). overrides_ writers also hold mutex_,
    // so the map read here is implicitly lock-protected. Zero-cost hash miss
    // when the slot has no override — safe in the hot parse path.
    auto it = overrides_.find(slot_index);
    if (it == overrides_.end()) {
        return;
    }
    const auto& o = it->second;
    // Merge policy — same as AD5X IFS. Override wins only when the override
    // field carries a real value; defaults fall through to firmware.
    if (!o.brand.empty())
        slot.brand = o.brand;
    if (!o.spool_name.empty())
        slot.spool_name = o.spool_name;
    if (o.spoolman_id > 0)
        slot.spoolman_id = o.spoolman_id;
    if (o.spoolman_vendor_id > 0)
        slot.spoolman_vendor_id = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0.0f)
        slot.remaining_weight_g = o.remaining_weight_g;
    if (o.total_weight_g >= 0.0f)
        slot.total_weight_g = o.total_weight_g;
    if (o.color_set)
        slot.color_rgb = o.color_rgb;
    if (!o.color_name.empty())
        slot.color_name = o.color_name;
    if (!o.material.empty())
        slot.material = o.material;
}

void AmsBackendSnapmaker::check_hardware_event_clear(SlotInfo& slot, int slot_index,
                                                     const std::string& observed_uid) {
    // Empty UID = "no reading" (no tag, unread, RFID reader disabled,
    // malformed CARD_UID). Treat as non-signal: don't update the baseline,
    // don't clear. Without this guard every tag-less poll would overwrite a
    // real prior UID and mask a genuine hardware swap on the next good read.
    if (observed_uid.empty())
        return;

    auto it = last_rfid_uid_.find(slot_index);
    if (it == last_rfid_uid_.end()) {
        // First observation for this slot — establish baseline. Even if the
        // override was previously saved against a different UID, the first
        // observation is NEVER a swap signal. apply_overrides still runs
        // after us and the override wins.
        last_rfid_uid_[slot_index] = observed_uid;
        spdlog::debug("{} Slot {} baseline RFID UID: {}", backend_log_tag(), slot_index,
                      observed_uid);
        return;
    }
    if (it->second == observed_uid)
        return; // unchanged — no swap signal

    // UID changed. Record the new UID as the baseline FIRST so a failed
    // clear_async doesn't make us re-fire on every subsequent poll.
    const std::string old_uid = it->second;
    it->second = observed_uid;

    auto ovr_it = overrides_.find(slot_index);
    if (ovr_it == overrides_.end()) {
        spdlog::debug("{} Slot {} RFID UID changed {} -> {} (no override to clear)",
                      backend_log_tag(), slot_index, old_uid, observed_uid);
        return;
    }

    spdlog::info("{} Slot {} RFID UID changed {} -> {}, clearing override "
                 "(physical spool swap detected)",
                 backend_log_tag(), slot_index, old_uid, observed_uid);

    // Delegate the erase + field reset + clear_async to the shared helper so
    // hardware-event clears and user-initiated clears share one field-reset
    // policy. Caller already holds mutex_.
    (void)ovr_it; // erased inside clear_override_locked
    clear_override_locked(slot_index, slot);
}

void AmsBackendSnapmaker::clear_override_locked(int slot_index, SlotInfo& slot) {
    // Caller must hold mutex_. Erases the in-memory override, resets STRICTLY
    // override-exclusive fields on the live SlotInfo so the cleared state is
    // visible in the very next get_slot_info() read (apply_overrides is a
    // no-op for this slot afterwards).
    //
    // Snapmaker field policy: brand / spool_name / total_weight_g come from
    // the RFID tag in handle_status_update — we must NOT zero those here or
    // we'd wipe newly-parsed firmware metadata. The override's copies of
    // those fields disappear with the erase; firmware's copies stay.
    // (color_name is not firmware-populated for Snapmaker — RFID has no
    // color-name field — so it's override-exclusive and gets cleared.)
    overrides_.erase(slot_index);

    slot.spoolman_id = 0;
    slot.spoolman_vendor_id = 0;
    slot.remaining_weight_g = -1.0f;
    slot.color_name.clear();

    if (override_store_) {
        // Capture by value only — clear_async's Moonraker callback can fire
        // after this function returns (MR tracker ~60s) and potentially
        // after the backend itself is gone. Same rationale as save_async.
        const std::string tag = backend_log_tag();
        override_store_->clear_async(slot_index, [tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} clear_async failed for slot {}: {}", tag, slot_index, err);
            }
        });
    }
}

void AmsBackendSnapmaker::clear_slot_override(int slot_index) {
    if (auto err = validate_slot_index(slot_index); !err.success()) {
        spdlog::warn("{} clear_slot_override: invalid slot {}", backend_log_tag(), slot_index);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot =
            system_info_.units.empty() ? nullptr : system_info_.units[0].get_slot(slot_index);
        if (!slot) {
            spdlog::warn("{} clear_slot_override: no slot entry for index {}", backend_log_tag(),
                         slot_index);
            return;
        }
        spdlog::info("{} Slot {} override cleared by user request", backend_log_tag(), slot_index);
        clear_override_locked(slot_index, *slot);
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
}

// ============================================================================
// Internal Helpers
// ============================================================================

AmsError AmsBackendSnapmaker::validate_slot_index(int slot_index) const {
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_TOOLS - 1);
    }
    return AmsErrorHelper::success();
}

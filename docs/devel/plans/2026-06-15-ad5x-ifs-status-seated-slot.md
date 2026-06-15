# AD5X IFS: IFS_STATUS as seated-channel authority + unload phase fix

Bug source: raza616 bundle **D8Z7DAA6** (v0.99.79, 2026-06-15). Fresh ring-buffer log.

## Symptoms (field)
1. When the extruder is loaded, the AMS context menu offers **"Unload" on all channels** (should be Unload only on the active channel, Eject on the rest).
2. Choosing "Unload" on a non-active channel (#2) **cut & removed the active filament (#4)**.
3. After that, the AMS action is **stuck on "Heating"** forever (nozzle not actually heating).
4. Misc GUI refresh lag (weaker evidence; out of ring buffer — not addressed here).

## Root cause (verified against log + code)
v0.99.79 already routes non-active unloads to cold `eject_lane()` (`unload_filament` :1059-1063)
and offers Eject via `can_unload_from_toolhead()`. Both hinge on `system_info_.current_slot`.
The defect: **`current_slot == -1` (active slot unknown) while a filament is physically seated.**

Trigger: after a load, every `GET_ZCOLOR` reports `Extruder: None (4)` — zmod's *idle* extruder
view (nothing actively feeding; last channel 4). `apply_zcolor_result` derives
`active_tool_`/`current_slot` only from the `N:` active-feed form (`extruder_slot`); "None"
→ `extruder_slot=nullopt` → `active_tool_=-1` → `current_slot=-1` (:2802-2811 → :367-375).
The `(4)` is parsed into `current_channel` but **never used**. Meanwhile `head_filament_`
(RS-485) stays true. That `head_filament_=true` + `current_slot=-1` combination:
- makes `can_unload_from_toolhead(any)` true via the `(head_filament_ && current_slot<0)`
  recovery branch (:957-959) → Unload on every slot;
- defeats the non-active→eject guard (`current_slot>=0` false, :1059) → `_IFS_REMOVE_CURRENT_PRUTOK`
  removes the actually-seated filament.

Proof `(N)` is stale, not seated-truth: `Extruder: None (4)` still appears at 6:44/6:45 **after**
port 4 was removed at 6:40.

Stuck-Heating (independent): unload phase machine `apply_phase_action_locked` is gated
`if (!reached_target_once) synth = HEATING` (:3198). `_IFS_REMOVE_CURRENT_PRUTOK` runs with
`BYPASS_TEMPERATURE_CHECK` and the unload path sends no preheat, so the nozzle never heats →
`reached_target_once` never true → stuck in HEATING even after head-drop. Only the 300s timeout
recovers. On old-zmod, `GET_ZCOLOR SILENT=1` returns a prompt dialog → `is_prompt_fallback`,
parse bails `saw_valid_response=false` (:2864-2867) → the zcolor finalize-to-IDLE can never fire.

## Fix: IFS_STATUS `Chan` as seated-channel authority
zmod 1.7.1 `cmd_IFS_STATUS` → `respond_info(json.dumps(get_values()))`, one JSON line:
`{"RawData":..,"State":int,"Ports":[bool×N],"Silk":bitmask,"Chan":int(1-based,0=none),
"Insert":..,"NeedInsert":bool,"Stall":bool,"stall_state":int}` (firmware
`zmod_ifs.py` IfsData.get_values :1372). **`Chan` = current active/engaged port** — persists
while loaded-idle, distinct from the "Extruder:" feed line. It's clean JSON via `respond_info`
(NOT a prompt dialog) → works on raza's old-zmod where GET_ZCOLOR degrades to prompt-fallback.

### Scope (surgical — do NOT touch presence authority)
GET_ZCOLOR remains presence authority (`port_presence_`). IFS_STATUS only supplies:
(a) the seated channel → `active_tool_`/`current_slot`; (b) a clean unload-finalize signal.
Do not use IFS_STATUS Ports/Silk for presence this round (leave the just-merged
presence-resurrection logic intact).

### Implementation
1. **Struct** `ZColorSilentResult` (`ams_backend_ad5x_ifs.h:214`): add
   `std::optional<int> ifs_chan;` (1-based; 0 means none). Distinct from the stale
   `current_channel` (leave that as-is, still unused).
2. **Send**: in `query_zcolor_silent()` (:2659), after sending `GET_ZCOLOR SILENT=1`, also
   `api_->execute_gcode("IFS_STATUS")` (fire-and-forget). `zcolor_query_active_` is already
   true, so the JSON line lands in `zcolor_response_buffer_`.
3. **Parse** `parse_zcolor_silent()` (:2849): BEFORE the `action:prompt_` early-return, scan for
   a line whose `// `-stripped content is a JSON object containing `"Chan"`. Parse with `json`,
   set `result.ifs_chan = obj["Chan"]`, `result.saw_valid_response = true`. **Reorder so the
   prompt early-return does not discard IFS_STATUS data.**
4. **Apply** `apply_zcolor_result()` (:2726): apply ifs_chan-derived state **before/independent
   of** the `is_prompt_fallback` early-return so prompt-fallback can't skip it. When
   `!has_ifs_vars_` and `ifs_chan` present: `*ifs_chan>0` → `active_tool_ =
   find_first_tool_for_port(*ifs_chan)`; `*ifs_chan==0` → `active_tool_=-1`. ifs_chan takes
   precedence over extruder_slot. Recompute `system_info_.current_slot` immediately via a new
   `recompute_current_slot_locked()` helper (factor out :367-375; call from both sites).
5. **Finalize** (:2822): for unload, `reached_end = !extruder_slot.has_value() ||
   (ifs_chan && *ifs_chan==0)`; for load, `... || (ifs_chan && *ifs_chan>0)`. Ensure the
   finalize path is reachable on prompt-fallback when ifs_chan is present.
6. **Phase machine**: in `on_head_transition_locked` (:3162), when `seen_head_drop` becomes true
   during an unload, also set `phase_tracker_.reached_target_once = true` so
   `apply_phase_action_locked` advances HEATING→UNLOADING without a heat event.
7. **Logging**: log parsed `Chan`/`Ports`/`State` at info so the next bundle proves Chan behavior.

## Field confirmation REQUIRED (ships blind, no AD5X here)
The fix assumes `Chan` persists at the loaded port while idle and goes 0 after unload. Verify:
raza runs `IFS_STATUS` (a) filament loaded + idle, (b) after unload, and pastes both JSON
outputs. The added logging also captures it in the next bundle.

## Tests
`tests/unit/test_ams_backend_ad5x_ifs.cpp`:
- IFS_STATUS JSON with `Chan:4` while GET_ZCOLOR says `Extruder: None (4)` → `current_slot==3`,
  `can_unload_from_toolhead(3)==true`, `can_unload_from_toolhead(0/1/2)==false`.
- `unload_filament(1)` with Chan=4 seated → routes to `eject_lane` (NOT `_IFS_REMOVE_CURRENT_PRUTOK`).
- IFS_STATUS `Chan:0` after head-drop during a tracked unload → action returns to IDLE.
- head-drop during unload sets reached_target_once → action advances past HEATING.

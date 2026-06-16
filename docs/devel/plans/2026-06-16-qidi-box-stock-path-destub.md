# QIDI Box Stock-Firmware Path — De-Stub Plan (#1022 / #1030)

**Status:** Planning · **Created:** 2026-06-16 · **Tracking issue:** **#1041** (stock-path de-stub + data collection) · refs #1022 (verify), #1030 (firmware dumps)

> **Correction (2026-06-16, after reading `.claude/scratchpad/qidi-box-firmware/`):** two rows below were wrong. `apply_filas_list()` **is implemented** (`ams_backend_qidi.cpp:536`, tested in `test_ams_backend_qidi.cpp`) — S3's real gap is consuming its output in the UI, not the parser. And the per-slot storage model for S2 is **known** from `saved_variables.cfg`: `filament_slot<n>` (→`[fila<n>]`), `color_slot<n>` (→`[colordict]`), `vendor_slot<n>` (→`[vendor_list]`), with the load gate `enable_box==1`. `set_slot_info()` just needs the inverse `SAVE_VARIABLE` writes — no longer device-blocked on "where is it stored", only on confirming Camden's specific values.
**Trigger:** Camden-Winder field test on **QIDI Q2 + Box, STOCK QIDI firmware (no Happy Hare), v0.99.79** (issue #1022, 2026-06-16). First real test of the *stock* path; most of it is stubbed or broken.

> The Happy Hare path is verified working on-device. Everything below is the **stock-firmware** path, which has never had a successful hardware test.

---

## 1. Corrected root-cause record (supersedes the first triage pass)

A subagent investigation initially concluded the QIDI backend "isn't selected on stock firmware" because detection keys solely on `box_stepper slot*`. **That is wrong** — Camden's own `/printer/objects/list` dump (issue comment 2026-06-15T01:41:56Z) contains:

- `box_stepper slot0`, `slot1`, `slot2`, `slot3`  ✅ (detection signal present)
- **no** bare `mmu` object (Happy Hare fully gone — so no higher-priority MMU suppresses QIDI in the `else if` chain)
- `heater_generic heater_box1`, `aht20_f heater_box1`, `box_extras`  ✅

So `PrinterDiscovery` sets `mmu_type_ = QIDI_BOX` (`include/printer_discovery.h:377`), `AmsBackendQidi` is instantiated, and Camden can see slots / attempt loads — confirming the backend is live. **Detection is not the bug.**

### The genuine open puzzle: dryer indicator never appears

At the C++ data layer the dryer indicator is **forced visible** for any drying-capable backend:

- `ams_backend_qidi.cpp:86` — `dryer_info_.supported = true;` (unconditional)
- `ams_backend_qidi.cpp:802` `get_dryer_info()` returns it verbatim (`supported` never flipped)
- `ams_state.cpp:1255` — `const int ind_vis = (has_env || dryer.supported) ? 1 : 0;`

`dryer.supported == true` ⇒ `ind_vis == 1` regardless of whether environment data exists. So the indicator **should** render — and it *did* render on the same display under Happy Hare. Yet Camden reports "no heater or temp icon" on the stock path. **Static analysis cannot explain this contradiction.** Candidate failure points, none confirmable from source alone:

1. `get_system_info().units` is empty (or `unit.unit_index ∉ [0, MAX_UNITS)`) at the moment `sync_from_backend()` runs the indicator loop (`ams_state.cpp:1185`), so `env_ind_visible_[idx]` is never set away from its init `0`. (Slots may render from `total_slots`/`slot_colors_` independently, so visible slots don't prove a non-empty `units`.)
2. The env-indicator widget isn't placed in the AMS-panel layout the Q2 actually uses, and Happy Hare happened to surface drying state through a different widget.
3. A subject-registration / refresh-timing gap specific to the QIDI unit.

**→ This item is diagnostic-gated. We need a device `-vv` log + screenshot before writing a fix (see §4, Data Request D1). Do not "fix" it blind — the code says it works.**

---

## 2. Stub inventory → real functionality

| # | Symptom (Camden) | Current code | Real implementation target | Data source |
|---|---|---|---|---|
| S1 | Dryer screen / heat-waves icon never shows | indicator *forced* visible; contradiction | Reproduce via device log, then fix the real gap | **Camden D1** (`-vv` log + screenshot) |
| S2 | "Spool info" → **"Feature not available"** | `set_slot_info()` returns `not_supported` (`ams_backend_qidi.cpp:756`) | Inverse `SAVE_VARIABLE` writes: `filament_slot<n>`/`color_slot<n>`/`vendor_slot<n>` (model known from `saved_variables.cfg`). Needs name→id reverse lookup into the parsed filas list. | **mostly unblocked**; D2 only confirms Camden's values |
| S3 | Spool list ignores `officiall_filas_list.cfg` / Spoolman (#1030) | `apply_filas_list()` **is implemented** (`ams_backend_qidi.cpp:536`); parses temps into `fila_profiles_`. Gap: parser keeps only temps (not name/type/color); output not surfaced as a material picker. | Extend `FilaProfile` to retain `filament`/`type`; expose as material presets; optional Spoolman bridge | **unblocked** (have format + sample); D3 seeds the real list |
| S4 | Load filament: heats to 250 °C, loading tracker shows, **then nothing**; no Eject | `load_filament()` sends `T<n>` (`ams_backend_qidi.cpp:688`); `unload_filament()` sends `UNLOAD_T<n>` (:720) | Send the macro the **stock** firmware actually implements for a box load/unload; track completion | **Camden D4** (`gcode_macro` list + console transcript of a manual load) + `box_stepper.py` |
| S5 | No Eject option in slot context menu | `supports_lane_eject()` not overridden → defaults `false` (`ui_ams_context_menu.cpp:251`) | If stock exposes a discrete eject/unload-to-box, implement `eject_lane()` + override the cap | depends on D4 |
| S6 | (latent) initial dryer/humidity snapshot missing | `on_started()` queries only `save_variables` + `box_extras` (`ams_backend_qidi.cpp:114`); no initial `heater_box`/`aht20_f` query — first values arrive only on next push delta | Add `heater_generic heater_box<N>` + `aht20_f heater_box<N>` to the `on_started` snapshot query | code-only |
| S7 | (low pri) `recover()`/`reset()`/`cancel()` stubbed | return `not_supported` (`ams_backend_qidi.cpp:739-752`) | Map to stock equivalents if they exist; otherwise leave honestly unsupported + hide UI affordance | D4 |

`start_drying`/`stop_drying` are **not** in this list — Camden already verified the Happy Hare gcode on-device, and the stock `ENABLE_BOX_DRY`/`DISABLE_BOX_DRY` path is wired (pending the S1 visibility fix to actually reach it).

---

## 3. Phasing

**Phase 0 — Lock the stock command surface. ✅ DONE (2026-06-16).**
The stock `box_stepper.so`/`box_extras.so` are *compiled* (no Python source in QIDITECH/klipper), but a real Q2+Box config ([`elliotboney/my_qidi_2`](https://github.com/elliotboney/my_qidi_2)) gave us the command surface. Findings folded into `QIDI_BOX_HEATER.md` ("Filament Operations (Stock Firmware)", "Filas List Format", "Known Unknowns"):
- **Load/unload:** `T<n>`→`EXTRUDER_LOAD SLOT=<slotname>`, `UNLOAD_T<n>`→`EXTRUDER_UNLOAD SLOT=<slotname>`, **both gated on `save_variables.enable_box == 1`** and using `value_t<n>` to map tool→slotname. HelixScreen's existing `T<n>`/`UNLOAD_T<n>` sends are correctly *named* — S4's "nothing happens" is almost certainly the `enable_box`/`value_t<n>` gate falling through, not a wrong command.
- **`officiall_filas_list.cfg`** (note double-l) format fully captured — INI `[filaN]`/`[colordict]`/`[vendor_list]`.
- **No discrete eject** command exists in stock config → S5 likely stays unsupported (confirm against Camden's macro list).
- **Still open:** where per-slot type/color is persisted (no `SAVE_VARIABLE` for it in `box1.cfg`; suspect `box_rfid.so`) — needs D2.

**Phase 1 — Diagnose S1 (device-gated).** Send Camden the D1 request. Triage the log against the three candidate failure points in §1. Only then write the indicator fix. (Likely small: e.g. ensure `get_system_info().units` is populated for QIDI before the first `sync_from_backend`, or place the env indicator in the Q2 layout.)

**Phase 2 — Filament storage (S2 + S3).** Implement `apply_filas_list()` (parser + `fila_profiles_` → presets/ranges) and `set_slot_info()` (write per-slot type/color through the stock variable store). These share the same data model and are the highest user-visible win after S1. Unit-test the parser against the real file from D3.

**Phase 3 — Load/unload/eject (S4 + S5).** Replace the `T<n>`/`UNLOAD_T<n>` sends with the verified stock macro from Phase 0 / D4. Implement `eject_lane()` + cap override only if a discrete command exists. Wire load-completion tracking so the loading overlay clears.

**Phase 4 — Polish (S6 + S7).** Initial-snapshot query; map or honestly disable recover/reset/cancel.

MAJOR work (4+ files, backend behavior, critical-ish path) → worktree, test-first on the parsers, review before merge. Each phase is independently shippable behind the already-removed write gate.

---

## 4. Data request for Camden (paste into #1022)

> Thanks for the stock-path run — that's the first real test of it, and it surfaced exactly what we needed. A few targeted asks so we replace the stubbed bits with the real thing instead of guessing. All read-only / safe with no print active; replace `<printer>` with the printer's IP.

**D1 — why the dryer controls don't appear (most important).**
Run HelixScreen with debug logging and reproduce "open the Multi-Filament/AMS panel, no heat-waves icon":
```sh
# in helixscreen.env, then restart HelixScreen:
HELIX_LOG_LEVEL=debug
```
Then grab the log and a screenshot of the AMS panel. In the log we're looking for the lines around AMS detection and `sync_from_backend` — specifically whether it logs creating the **QIDI Box** backend and how many units it reports.

**D2 — saved state (fixes both "Feature not available" *and* the 250 °C-then-nothing load).**
```
http://<printer>/printer/objects/query?save_variables
```
This one dump answers a lot. We specifically want to see:
- `enable_box` (if it's not `1`, that's why `T<n>` loads do nothing — the macro is gated on it)
- `value_t0`..`value_t3` (the tool→slot mapping)
- whether per-slot **filament type/color** is stored here at all (we suspect it lives in the box's RFID state instead).
Then, if you set a slot's filament type/color in QIDI's own screen, do it for one slot and re-run the query so we can see exactly which variable changes (that's what `set_slot_info` needs to write).

**D3 — material list (#1030). (We already have the *format* from a public Q2 config — we just want YOUR file to seed the real list.)**
The contents of `officiall_filas_list.cfg` (yes, double-l) and `drying.conf`, from `~/printer_data/config/`.

**D4 — confirm the load gate + eject.**
1. Macro list (to confirm your firmware's macros match what we found):
   ```
   http://<printer>/printer/objects/list
   ```
2. We believe load is `T<n>` → `EXTRUDER_LOAD SLOT=slot<n>`, gated on `enable_box == 1`. In the Fluidd/Mainsail **console**, try running `EXTRUDER_LOAD SLOT=slot0` directly (no print active) and tell us if the filament actually loads — that confirms whether the gate is the problem vs. the command.
3. Is there any way in QIDI's own UI to **eject/unload a single slot back to the box** (separate from a full filament change)? If so, what command does it run?

---

## 5. Key file references

- Backend: `src/printer/ams_backend_qidi.cpp`, `include/ams_backend_qidi.h`
- Detection: `include/printer_discovery.h:377` (`box_stepper slot*`)
- Indicator visibility: `src/printer/ams_state.cpp:1184-1265` (gate at `:1255`)
- Dryer overlay: `src/ui/ui_ams_environment_overlay.cpp`, `ui_xml/ams_environment_overlay.xml`
- Eject cap gate: `src/ui/ui_ams_context_menu.cpp:251`
- Subscription wiring: `src/api/moonraker_discovery_sequence.cpp` (~999 sensor fields, ~1468 `aht20_f` classify)
- RE reference: `docs/devel/QIDI_BOX_HEATER.md`

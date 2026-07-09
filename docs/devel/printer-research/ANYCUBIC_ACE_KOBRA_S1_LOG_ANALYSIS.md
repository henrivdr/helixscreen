# Anycubic ACE Pro on Kobra S1 — Real-World Log Analysis

> **Source:** `klippy.log` + `moonraker.log` from a community Kobra S1 running a
> **Python Klipper fork** (not KobraOS), captured 2026-06-24. Logs were taken with
> the ACE connected but **no print running** — a second capture *with* a print is
> pending (see [Open Items](#open-items)).
>
> This is a companion to [`ANYCUBIC_KOBRA_COREXY_RESEARCH.md`](ANYCUBIC_KOBRA_COREXY_RESEARCH.md).
> Where the two disagree, **this file reflects observed log data**; the research doc
> is partly extrapolated from KobraOS/Rinkhals and the native `filament_hub` object.

---

## TL;DR — why this setup matters

This is **not** the KobraOS/GoKlipper path that
[`ams_backend_ace.cpp`](../../../src/printer/ams_backend_ace.cpp) currently keys on
(`status.filament_hub` / `status.ace`). It is a **mainline-Python Klipper fork**
(`github.com/Kobra-S1/klipper-kobra-s1`, branch `Kobra-S1-Dev`) running on a
**Raspberry Pi 5**, talking to the ACE over its own USB-serial protocol via a custom
`[ace]` Klipper extra, and exposing front-end state through a custom
**`[ace_status]` Moonraker component**. It is *related* to the generic "any Klipper
printer + ACE driver" approach (cf. ValgACE, research doc §10) but is **not** that —
it is a Kobra-S1-specific firmware fork (see [Scope](#scope-fork-specific-vs-portable-driver)).

**Good news for HelixScreen:** the control surface is gcode (`ACE_CHANGE_TOOL`,
`ACE_ENABLE/DISABLE_FEED_ASSIST`, `T0..T7`), which our backend already emits.
**Open question:** the *status* surface (`[ace_status]` JSON shape / REST endpoints)
is unconfirmed and may differ from both `filament_hub` and our assumed
`/server/ace/*` REST endpoints. That gap is the main integration risk.

---

## Scope: fork-specific vs portable driver

Two separable things are bundled in these logs, and only one is portable.

**The ACE driver is portable (in principle).** The `[ace]` Klipper extra and the
`ace_status.py` Moonraker component just speak the ACE's USB-CDC JSON protocol —
nothing about *that* layer is tied to a particular printer. This is the same role
ValgACE fills as a bolt-on for vanilla Klipper.

**This distribution is not — it is purpose-built for the Kobra S1.** The fork ships
the ACE driver wrapped in macros that hardcode **KS1 physical hardware**:

| KS1-specific assumption | Where |
|-------------------------|-------|
| Throw/poop area at **X48, Y230–276** | `TO_THROW_POSITION`, `TO_BLADE`, `MOVE_HEAT_POS` |
| Physical **tip cutter at X261, Y12** (Y-jab into a real blade) | `CUT_TIP` |
| Dual-MCU topology: main `/dev/ttyGS0` **+ separate `nozzle_mcu` `/dev/ttyGS1`** | `[mcu]`, `[mcu nozzle_mcu]` |
| CoreXY kinematics, KS1 bed extents / soft limits | `[printer]`, macro clamps |
| Feed geometry (park→toolhead 900 mm, etc.) | `[ace]` (§5) |

Run this fork unchanged on a non-KS1 machine and *filament load* would work, but
`CUT_TIP` / `TO_THROW_POSITION` / purge moves would drive the toolhead to coordinates
and hardware (the cutter) that don't exist there. So **the package targets Kobra S1
hardware**, even though the embedded ACE protocol driver is machine-agnostic.

**"On the printer" ≠ "on the printer's CPU."** In these logs the fork runs on an
**external Raspberry Pi 5** wired to the KS1's mainboard, toolhead board, and ACE over
USB — KobraOS removed, mainline Klipper in its place. The *host* can be external; the
*target hardware* is still a Kobra S1.

### Three distinct ACE platforms for HelixScreen

This is why "ACE support" is not one thing. At least three integration surfaces exist:

| Platform | Klipper | ACE status surface | Notes |
|----------|---------|--------------------|-------|
| **KobraOS / GoKlipper** (stock + Rinkhals) | Go rewrite | native `filament_hub` / `ace` printer object | what [`ams_backend_ace.cpp`](../../../src/printer/ams_backend_ace.cpp) currently parses |
| **KS1 mainline-Klipper fork** (these logs) | Python fork `klipper-kobra-s1` | custom `[ace_status]` component (shape **TBD**) | KS1-specific macros; external or on-board host |
| **ValgACE on any Klipper printer** | vanilla Python | its own `ace_status.py` (shape **TBD**) | user supplies cutter/purge macros for their machine |

Whether one backend code path covers all three depends entirely on how similar those
status surfaces are — currently unknown (see [Open Items](#open-items)).

---

## 1. Host / firmware identity

| Item | Value (observed) |
|------|------------------|
| Klipper fork | `github.com/Kobra-S1/klipper-kobra-s1`, branch `Kobra-S1-Dev`, `V1.2-40-gee23c0ab-dirty` |
| Host | Raspberry Pi 5, Linux 6.18.x aarch64, Python 3.13 |
| Custom Klipper extras (untracked) | `temperature_ace.py`, `virtual_pins.py`, `flow_calibration.py` |
| Custom Moonraker component (untracked) | `moonraker/components/ace_status.py` |
| Front-ends | Mainsail + Fluidd; Spoolman (`spoolman.shoikan.org`) |
| HelixScreen | registered as Moonraker `update_manager` web client (`prestonbrown/helixscreen`) |

This is the "tunneled vanilla" arrangement: real Klipper + Moonraker on a Pi, not the
printer's stock Rockchip board.

---

## 2. ACE device & connection (from `GET_INFO`)

Observed connect handshake (`klippy.log`):

```
ACE[0] USB device found: /dev/ttyACM0 at location '4-2.3'
ACE[0]: Serial port /dev/ttyACM0 opened
ACE[0]: Heartbeat started (interval=1.0s)
ACE[0]: GET_INFO raw_info: {"code": 0, "id": 2, "msg": "success", "result":
  {"boot_firmware": "V1.0.1", "firmware": "V1.3.863", "id": 1,
   "model": "Anycubic Color Engine Pro", "slots": 4, "structure_version": "0"}}
```

| Field | Value | Notes |
|-------|-------|-------|
| `model` | `Anycubic Color Engine Pro` | canonical identity string |
| `slots` | `4` | per unit |
| `firmware` | `V1.3.863` | ACE main fw |
| `boot_firmware` | `V1.0.1` | bootloader |
| `structure_version` | `"0"` | unknown semantics |

**Refines the research doc:**
- Port: selected by **USB physical topology** (stores location tuple `(4,2,3)`,
  re-selects same physical port on reconnect — "topology match"), *not* by a
  `by-id` symlink. The `/dev/serial/by-id/usb-ANYCUBIC_ACE_0-if00` form in the
  research doc was not observed here.
- Baud: `[ace] baud = auto` (research doc's "115200" is the KobraOS figure).
- Connection liveness: **1 s heartbeat**; `ace_connection_supervision = False` here.

---

## 3. Klipper wiring

Three pieces make the ACE controllable from gcode:

- **`[ace]`** — the driver extra. Registers the runtime gcode commands and holds all
  feed/retract/purge geometry (see §5). `ace_count = 1` → tools `T0–T3`; a second
  unit would add `T4–T7` (8 max).
- **`[temperature_ace]` + `[temperature_sensor ace_temp]`** — exposes the ACE dryer
  temperature as a normal Klipper sensor (`ace_instance=0`, 0–70 °C). Read `0.0`
  here (dryer off).
- **`[virtual_pins]` + `[output_pin ACE_Pro] value=1`** — software master-enable flag.
  **Every ACE macro early-outs on `{% if printer["output_pin ACE_Pro"].value %}`.**
  HelixScreen should treat this pin as the "ACE present/enabled" gate.

---

## 4. Command surface (integration API)

All driven through `execute_gcode` — already how `ams_backend_ace.cpp` operates.

| Command | Purpose |
|---------|---------|
| `T0` … `T7` | Select tool N. Each does `_SET_SPOOL_BY_TOOL TOOL=N` (Spoolman sync) then `ACE_CHANGE_TOOL TOOL=N PURGELENGTH=…` |
| `ACE_CHANGE_TOOL TOOL=n [PURGELENGTH=…]` | Core load/change. `TOOL=-1` = unload only |
| `TR` | Shortcut for `ACE_CHANGE_TOOL TOOL=-1` (retract/unload) |
| `ACE_ENABLE_FEED_ASSIST` / `ACE_DISABLE_FEED_ASSIST` | Spool feed-assist motor toggle |

A tool change is bracketed by Anycubic macros (not commands HelixScreen calls
directly, but useful for understanding timing/observable side effects):

- `_ACE_PRE_TOOLCHANGE` — save temp/fan, Z-hop ≥4 mm, fan off, move to throw
  position (front-left, ~X48 Y230–276), heat to purge temp.
- `_ACE_PREPARE_FOR_RETRACTION` + `CUT_TIP` — 2 mm pre-cut retract, then a physical
  **tip cut at the front-right cutter** (X261, Y-jab), extruder wiggle to free tip.
- `_ACE_POST_TOOLCHANGE` — `PURGE_IN_CHUNKS` (≤250 mm chunks → `PURGE_AND_POOP`),
  nozzle wipe, restore fan + temp, return to print.

---

## 5. `[ace]` geometry / tuning (observed defaults)

Useful for a settings/diagnostics UI and for understanding feed timing. **Config-declared
values — not yet observed in motion.**

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `feed_speed` / `retract_speed` | 90 / 75 | ACE spool motor (mm/s) |
| `total_max_feeding_length` | 2600 | hard cap, spool→toolhead push |
| `parkposition_to_toolhead_length` | 900 | park (staged) → extruder gears |
| `parkposition_to_rdm_length` | 150 | park → mid-path RDM sensor |
| `rdm_overshoot_length` | 50 | |
| `toolchange_load_length` | 920 | bulk feed on a tool change |
| `toolhead_full_purge_length` | 85 | |
| `extruder_feeding_length` / `_speed` | 10 / 8 | short bite handing filament to extruder gear |
| `toolhead_slow_loading_speed` | 5 | final approach into hotend |
| `incremental_feeding_length` / `_speed` | 100 / 60 | |
| `pre_cut_retract_length` | 2 | |
| `default_color_change_purge_length` / `_speed` | 50 / 300 | |
| `purge_max_chunk_length` | 250 | purge split granularity |
| `feed_assist_active_after_ace_connect` | True | |

**Feed model:** filament normally sits parked just before the toolhead; the ACE motor
does the long bulk feed (~920 mm), hands ~10 mm to the extruder gear, which makes the
final slow (5 mm/s) push into the hotend.

---

## 6. Filament sensors (two, different roles)

| Sensor | Pin | `pause_on_runout` | Role |
|--------|-----|-------------------|------|
| `filament_runout_nozzle` | `nozzle_mcu:PB0` | **True** | Toolhead sensor. On runout → park + Mainsail/Fluidd prompt dialogs; on insert → auto-feed 150 mm + Resume/Extrude/Cancel prompt |
| `filament_runout_rdm` | `PB0` (main mcu) | False | **RDM** mid-path sensor (150 mm from park), used by the `ace` driver for feed verification, not pausing |

Note the separate **`nozzle_mcu`** (`/dev/ttyGS1`) toolhead board distinct from the
main board (`/dev/ttyGS0`).

---

## 7. Slot inventory & RFID data model

### Persisted store
`[save_variables] filename = ~/printer_data/config/saved_variables.cfg`
(on this host: `/home/pi/printer_data/config/saved_variables.cfg`).

Inventory lives in:
- `ace_inventory_0` — slots 0–3 (unit 0)
- `ace_inventory_1` — slots 4–7 (unit 1)

Each entry is an object with at least `sku` and `status` fields (read by
`_SET_SPOOL_BY_TOOL` / `_SET_SPOOL_BY_SLOT`). Per-tool manual overrides also persist:
`manual_spool_id_<N>`, `manual_spool_lock_sku_<N>`.

Tool→spool resolution priority (from `_SET_SPOOL_BY_TOOL`): **RFID tag → manual
locked SKU → manual spool id → none**, then `spoolman_set_active_spool`.

### RFID (observed)
On connect the driver queries RFID per slot:

```
ACE[0]: Slot 0 - No RFID tag (rfid=1), skipping inventory update to preserve manual data
ACE[0]: Slot 1 - No RFID tag (rfid=0), ...
ACE[0]: Slot 2 - No RFID tag (rfid=1), ...
ACE[0]: Slot 3 - No RFID tag (rfid=1), ...
```

⚠️ **`rfid` is not a simple present/absent boolean** — both `rfid=0` and `rfid=1`
were reported as "No RFID tag". Meaning unresolved (see Open Items). **HelixScreen
must support manual SKU assignment**; do not assume RFID populates slots.

---

## 8. Moonraker side

- **`[ace_status]`** component loaded: `ACE Status API extension loaded`
  (`moonraker/components/ace_status.py`, untracked in the fork). This is the intended
  front-end status channel. **Its JSON/REST surface is not visible in these logs** —
  only that it loaded.
- **`[spoolman]`** → `spoolman.shoikan.org`, `sync_rate=1`.
- No `filament_hub` or `ace` printer-object status payload appears in these logs.

---

## 9. HelixScreen compatibility assessment

| Aspect | Status from these logs |
|--------|------------------------|
| Control (load/unload/change) | ✅ gcode `ACE_CHANGE_TOOL` / `T0..T7` — backend already emits this |
| Feed assist toggle | ✅ `ACE_ENABLE/DISABLE_FEED_ASSIST` present |
| Master enable detection | ✅ `output_pin ACE_Pro` value gates everything |
| Slot count / topology | ✅ 4 slots/unit, up to 8 tools |
| Dryer temp readout | ✅ via `temperature_sensor ace_temp` |
| Dryer **control** | ❓ no drying command observed in this fork (research doc cites `drying`/`drying_stop` on native protocol) |
| **Status payload shape** | ❌ **unknown** — `[ace_status]` surface not in logs; may not match `status.filament_hub`/`status.ace` that our backend parses, nor the assumed `/server/ace/*` REST endpoints |
| Live slot state (loaded/empty/type/color/humidity) | ❌ not in connect-only logs |

**Integration risk = the status surface.** Our backend reads slot state from
`filament_hub`/`ace` Klipper objects (KobraOS/GoKlipper native). This Python-fork
setup may instead serve slot state via the custom `[ace_status]` REST/JSON. Until we
see that shape, we can't confirm `ams_backend_ace.cpp` populates slots on this path.

---

## Open Items

> TODO items, highest-value first. The first two beat the pending print log for
> integration purposes.

- [ ] **TODO (highest value): obtain `moonraker/components/ace_status.py`** from this
  printer. Defines the exact JSON fields / REST endpoints HelixScreen would consume.
  Resolves whether our backend's `filament_hub`/`ace` parsing or a different shape
  applies. Cross-check against research doc §9 assumed endpoints
  (`/server/ace/info|status|slots`).
- [ ] **TODO (high value): obtain a populated `saved_variables.cfg`**
  (`/home/pi/printer_data/config/saved_variables.cfg`) to see a real `ace_inventory_0`
  entry — concrete `sku`/`status` values and any additional per-slot fields.
- [ ] **TODO: print-running log capture** (requested). Will surface: live status-push
  JSON, real tool-change sequencing/timing, feed-assist behavior under load,
  error/retry paths, and slicer-driven purge amounts.
- [ ] **TODO: resolve `rfid` flag semantics** — why `rfid=0` vs `rfid=1` both mean
  "no tag". Needs the `[ace]`/`ace_status` source or a tagged-spool capture.
- [ ] **TODO: confirm dryer control path** on this fork (is there a drying gcode/API,
  or read-only temp?).
- [ ] **TODO: confirm whether `ams_backend_ace.cpp` actually populates slots** against
  the `[ace_status]` surface, or needs a new code path for the Python-fork ACE.
- [ ] **TODO: decide if one backend covers all three ACE platforms** (KobraOS
  `filament_hub`, this KS1 fork's `[ace_status]`, ValgACE's `ace_status.py`) or whether
  they need separate parsing — depends on diffing their status surfaces. See
  [Scope → Three distinct ACE platforms](#three-distinct-ace-platforms-for-helixscreen).

---

*Generated from log analysis 2026-06-24. Logs were connect-only (no active print).*

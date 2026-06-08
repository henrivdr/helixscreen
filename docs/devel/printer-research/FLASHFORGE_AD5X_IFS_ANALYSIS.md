# FlashForge AD5X — IFS System Deep Analysis

**Date**: 2026-03-09
**Status**: Real device data analyzed (from user dump)
**Source**: Live AD5X running ZMOD, user-provided debug dump + Python source

---

## 1. Architecture Overview

The AD5X IFS (Intelligent Filament Switching) is a **separate MCU** communicating over **UART serial** (`/dev/ttyS4` at 115200 baud). It is NOT a Klipper MCU — it's a standalone board (FFP0202_IFS_Con_Board, STM32-based) that the `zmod_ifs.py` Klipper extra talks to via F-commands.

**Key insight**: There are NO dedicated Klipper objects for IFS state. The system is entirely macro-driven, with state stored in JSON files on disk and Klipper `save_variables`.

### Data Flow
```
IFS Board ←serial→ zmod_ifs.py ←gcode→ Klipper macros ←websocket→ Moonraker ←http→ HelixScreen
```

---

## 2. Serial Protocol (F-Commands)

The IFS board accepts commands and returns text responses. The `zmod_ifs.py` module polls `F13` every 200ms when idle.

| Command | Function | Response |
|---------|----------|----------|
| `F10 C{port} L{len} S{speed}` | Feed filament into port | `F10 ok. FFS channel {port} feeding.` |
| `F11 C{port} L{len} S{speed}` | Retract filament from port | `F11 ok. FFS channel {port} exiting.` |
<!-- gcode-layer note: F10/F11/F112 are surfaced at the gcode layer as IFS_F10/IFS_F11/IFS_F112 (registered in zmod_ifs.py). IFS_F11 defaults: PRUTOK=1 LEN=90 SPEED=1200 CHECK=0. CHECK=0 = no presence guard, no heating. -->
> **Surfaced at the gcode layer**: `F10`/`F11`/`F112` are registered as `IFS_F10`/`IFS_F11`/`IFS_F112`. `IFS_F11` defaults `PRUTOK=1 LEN=90 SPEED=1200 CHECK=0` — `CHECK=0` means no presence guard and no heating (an unconditional cold per-lane retract).
| `F13` | Status query | `FFS_state: N silk_state: N stall_state: N chan: N ffs_channels_insert: N` |
| `F15 C` | Driver reset | `F15 ok.` |
| `F18` | Release ALL clamps | `F18 ok` |
| `F23 C{port}` | Mark filament as inserted | `F23 ok. chan {port}.` |
| `F24 C{port}` | Clamp filament (grip) | `F24 ok. chan {port}.` |
| `F39 C{port}` | Release clamp for port | `F39 ok. FFS channel {port} release.` |
| `F112` | Force stop all movement | `F112 ok.` or `F112 ok. yes.` |

> **These F-commands ARE exposed as core ZMOD gcode commands.** They are registered in `zmod_ifs.py` via `gcode.register_command('IFS_F11', ...)` (and `IFS_F10`, `IFS_F112`, `IFS_F13`/`F15`/`F18`/`F23`/`F24`/`F39`) — verified in ZMOD AD5X v1.7.1, `mod/.shell/zmod_ifs.py:152`. They are **not** serial-only. `IFS_F11 PRUTOK={port} LEN={len} SPEED={speed}` is a thin wrapper over the raw serial command `F11 C{port} L{len} S{speed}`: its handler `cmd_IFS_F11` contains zero heating logic, and with the default `CHECK=0` it requires no lane presence/runout sensor to read filament — it waits only for a generic ready state. That makes `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` an **unconditional COLD per-lane retract**. Heating in the normal unload flow lives in the caller macro `_REMOVE_PRUTOK_IFS` (via `IFS_REMOVE_CURRENT_PRUTOK TEMP=…`), not in `IFS_F11` itself. By contrast, the toolhead-oriented unload macros (`REMOVE_PRUTOK_IFS`, `IFS_REMOVE_PRUTOK`) heat + retract the filament currently loaded to the toolhead — they do **not** jog an individual idle lane.

### F13 Status Response Fields

| Field | Type | Meaning |
|-------|------|---------|
| `FFS_state` | int | State machine value (see below) |
| `silk_state` | bitmask | Per-port filament presence (bit0=port1, bit1=port2, etc.) |
| `stall_state` | bitmask | Per-port motion stall detection |
| `chan` | int | Current active channel |
| `ffs_channels_insert` | bitmask | Which port just had filament physically inserted (triggers autoinsert) |

### FFS State Machine Values

States are offset by 11 per channel: `base + (channel-1) * 11`

| Value | Meaning |
|-------|---------|
| 3 | Polling/querying |
| 5 | **Ready** (idle) |
| 7, 18, 29, 40 | Clamped (per channel) |
| 11, 22, 33, 44 | **Loading** (per channel) |
| 12, 23, 34, 45 | Releasing clamp (per channel) |
| 15, 26, 37, 48 | **Unloading** (per channel) |
| 127 | Driver error |

---

## 3. Klipper Objects Available via WebSocket

### Standard Filament Sensors (subscribable)

Per-port presence sensors:
- `filament_switch_sensor _ifs_port_sensor_1` through `_4`
- `zmod_ifs_switch_sensor _ifs_port_sensor_1` through `_4` (zmod wrappers, same data)

Per-port motion sensors:
- `filament_motion_sensor _ifs_motion_sensor_1` through `_4`
- `zmod_ifs_motion_sensor _ifs_motion_sensor_1` through `_4`

Toolhead sensors:
- `filament_switch_sensor head_switch_sensor` — filament in extruder
- `zmod_ifs_switch_sensor head_switch_sensor`
- `filament_motion_sensor ifs_motion_sensor` — main motion sensor

### Other Notable Objects
- `temperature_sensor weightValue` — load cell (filament weight, type `temperature_load`)
- `temperature_sensor head` — extruder head temp sensor (separate from heater)
- `save_variables` — persistent variables (colors, types, tool mapping)

### Tool Macros
- `gcode_macro T0` through `T15` (16 defined, only 4 physical ports)
- `gcode_macro SET_EXTRUDER_SLOT` — sets active slot: `_SET_EXTRUDER_SLOT SLOT={slot}`

---

## 4. State Storage

### JSON Config Files (on device filesystem)

| File | Contents |
|------|----------|
| `/usr/prog/config/Adventurer5M.json` | `FFMInfo.channel` (active port), `FFMInfo.ffmType{1-4}` (material), `FFMInfo.ffmColor{1-4}` (hex color) |
| `/usr/data/config/mod_data/file.json` | Tool-to-port mapping array (e.g., `[1, 2, 3, 4]` maps T0→port1) |
| `/usr/data/config/mod_data/filament.json` | Per-material filament profiles (speeds, lengths, temps) |

### Klipper save_variables (`/usr/data/config/mod_data/variables.cfg`)

Key IFS-related variables from a real device:

```ini
allowed_tool_count = 4
less_waste_colors = ['000000', 'FFFFFF', '8000FF', '004000']
less_waste_types = ['PLA', 'PLA', 'PLA', 'PLA']
less_waste_tools = [1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]
less_waste_current_tool = -1
less_waste_e_feedrate = 810
less_waste_e_feedrates = [130, 130, 810, 130, ...]
less_waste_external = 0
less_waste_extruder_port = 0
less_waste_extruder_temp = 220
autoinsert = 1
head_switch_sensor = 0
ifs_motion_sensor = 0
```

**Note**: `less_waste_colors` are hex strings WITHOUT `#` prefix. `less_waste_current_tool = -1` means no tool active.

---

## 5. Key G-Code Commands

### Tool Change Flow
1. `A_CHANGE_FILAMENT CHANNEL={n}` — orchestrates full tool change (save position, retract old, load new, purge, restore)
2. `END_CHANGE_FILAMENT` — restores temperature, fan speed, position after change
3. `INSERT_PRUTOK_IFS PRUTOK={n}` — load filament from port N (toolhead load; looks up temp from config)
4. `REMOVE_PRUTOK_IFS PRUTOK={n}` — runs the **toolhead unload** sequence (heats + retracts the filament currently loaded to the toolhead). It is toolhead-oriented, **not** a per-port/per-lane jog: observed on a real AD5X (native ZMOD), `REMOVE_PRUTOK_IFS PRUTOK=3` with filament from a *different* slot loaded at the toolhead heated the hotend and unloaded the loaded filament, ignoring port N. On native ZMOD it can error `No filament N in IFS` when the IFS state disagrees with presence. Do **not** treat it as "unload filament from port N independently." For a cold per-lane retract of an idle lane, use the gcode-layer `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` instead (see §2) — it performs no heating and (with `CHECK=0`) requires no presence sensor.
5. `IFS_REMOVE_PRUTOK` — retract the currently-loaded toolhead filament

**Note on underscore variants**: `_INSERT_PRUTOK_IFS`, `_REMOVE_PRUTOK_IFS`, `_IFS_REMOVE_PRUTOK` are internal macros that expect an explicit `TEMP` parameter (default fallback: 220). The no-underscore public versions look up temperature from the config automatically. Always use the no-underscore versions.
6. `SET_EXTRUDER_SLOT SLOT={n}` → `_SET_EXTRUDER_SLOT SLOT={n}` — tell firmware which slot is active
7. `SET_CURRENT_PRUTOK` — detect and set active filament based on sensor state

### Status/Control
- `IFS_STATUS` — returns JSON: `{"State": N, "Port1": bool, "Port2": bool, "Port3": bool, "Port4": bool, "Silk": N, "Chan": N, "Insert": N, "NeedInsert": bool, "Stall": bool, "stall_state": N}`
- `IFS_UNLOCK` → `IFS_F18` — release all clamps
- `_IFS_ON` / `_IFS_OFF` — enable/disable IFS system
- `COLOR` — update color display
- `LOAD_MATERIAL` — interactive filament load (4-stage: SELECT → HEATUP → ACTION → END)

### Filament Load/Unload (Manual)
- `LOAD_FILAMENT` — manual load
- `UNLOAD_FILAMENT` — manual unload
- `PURGE_FILAMENT` — manual purge

---

## 6. Python Module: `zmod_ifs.py`

### Key Class: `zmod_ifs`
- Opens `/dev/ttyS4` serial port in a background thread (`_sensor_reader`)
- Polls `F13` every 200ms when idle
- Command/response uses incrementing ID system (`F10#42` → response matched by ID)
- Thread-safe via `_command_lock` and `_ret_command_lock`
- Auto-detects IFS availability (empty response = IFS offline → `_IFS_OFF`)
- Auto-insert: when `NeedInsert` becomes true (filament physically pushed into port), triggers `_IFS_AUTOINSERT`

### Key Class: `IfsData`
- Thread-safe state container updated from F13 responses
- `get_values()` returns full state dict (used by `IFS_STATUS` command)
- `get_port(n)` → bool for filament presence per port
- `get_stall(n)` → bool for motion stall per port

### Filament Config System
- Default temps: PLA=220, PLA-CF=220, SILK=230, TPU=230, ABS=250, PETG=250, PETG-CF=250
- Per-material profiles in `filament.json` with configurable speeds, tube lengths, purge amounts
- `DEFAULT_FILAMENT_SETTINGS` dict defines all tuneable parameters

---

## 7. Detection Strategy for HelixScreen

### How to Detect AD5X IFS

**Primary**: Look for `zmod_ifs_switch_sensor` OR `zmod_ifs_motion_sensor` in `printer.objects.list`

**Secondary confirmation**: Check for `gcode_macro SET_EXTRUDER_SLOT` and `gcode_macro _IFS_AUTOINSERT`

**NOT detectable via**: Standard AFC/Happy Hare/toolchanger object prefixes (none present)

### How to Get State

**Option A (Preferred)**: Subscribe to `save_variables` → watch `less_waste_colors`, `less_waste_types`, `less_waste_current_tool`, `less_waste_tools`

**Option A2 (Native ZMOD)**: Download `Adventurer5M.json` via Moonraker file API (`config` root). Parse `FFMInfo.ffmColor1-4` and `FFMInfo.ffmType1-4`. Re-read on port sensor changes or when `RUN_ZCOLOR`/`CHANGE_ZCOLOR` appears in gcode response stream.

**Option B**: Execute `IFS_STATUS` G-code command and parse JSON response for real-time hardware state (port presence, stall, active channel)

**Option C**: Subscribe to individual `filament_switch_sensor _ifs_port_sensor_{1-4}` for per-port presence

### How to Control

All operations via G-code commands:
- Tool change: `A_CHANGE_FILAMENT CHANNEL={n}`
- Load: `INSERT_PRUTOK_IFS PRUTOK={n}` (toolhead load from port N)
- Unload (toolhead): `REMOVE_PRUTOK_IFS PRUTOK={n}` or `IFS_REMOVE_PRUTOK` — both run the toolhead unload (heat + retract the currently-loaded filament). Neither jogs an idle lane; `PRUTOK=N` does **not** select an independent port to eject.
- Unlock: `IFS_UNLOCK`

> **Cold per-lane reverse-jog IS available at the gcode layer** via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (core ZMOD — no heat, no presence guard; see §2). `REMOVE_PRUTOK_IFS` / `IFS_REMOVE_PRUTOK` remain the heated *toolhead* unload, distinct from `IFS_F11`. See §12.

---

## 8. Backend Implementation Notes

### Topology
**Linear** — 4 independent lanes → single hub at toolhead (no hub splitter like AFC). Each lane has its own motor, clamp, and presence sensor.

### Slot Count
Fixed at 4 (from `allowed_tool_count` variable, but hardware is always 4 ports).

### Color/Material Info
Available from `save_variables`:
- Colors: `less_waste_colors` (list of hex strings, no `#` prefix)
- Materials: `less_waste_types` (list of material name strings)
- Tool mapping: `less_waste_tools` (index = tool number, value = physical port)
- Active: `less_waste_current_tool` (-1 = none)

### Caveats
1. **No Moonraker database storage** — state is in Klipper `save_variables` and device JSON files
2. **Colors are hex WITHOUT #** — need to prepend for our UI: `"8000FF"` → `0x8000FF`
3. **Tool numbering**: T0-T15 are macros but only 4 physical ports (1-4). The `less_waste_tools` array maps logical tools to physical ports.
4. **IFS availability is dynamic** — the serial connection can drop. `zmod_ifs.ifs` bool tracks availability. When unavailable, all commands call `_IFS_OFF`.
5. **Autoinsert**: Physical insertion of filament into a port auto-triggers loading sequence (configurable via `autoinsert` variable).

---

## 9. Macro Packages: bambufy vs lessWaste

Two IFS macro packages exist for ZMOD. Both use the same `save_variables` schema.

### bambufy (Original)
- **Repo**: [function3d/bambufy](https://github.com/function3d/bambufy)
- Stock IFS macro package, 4 tools (T0-T3), basic load/unload/purge

### lessWaste (Enhanced Fork)
- **Repo**: [Hrybmo/lessWaste](https://github.com/Hrybmo/lesswaste)
- Based on bambufy V1.2.10, adds significant features:
  - **16 virtual tools** (T0-T15) mapped to 4 physical ports via `variable_tools`
  - **Backup/failover**: `variable_backup` + `variable_backup_filament_spent` — auto-switch to matching color/type on runout
  - **Virtual channel mode**: `variable_is_virtual_mode` — allows more slicer tools than physical slots
  - **Purge control**: in-tower (`_NOPOOP`) or out-the-back, configurable flush volumes
  - **Same-filament purge skip**: `variable_same_filament_purge`
  - **Per-tool feedrates**: `variable_e_feedrates` array
  - **Auto-recovery**: `_CHECK_FILAMENT` macro detects which port is loaded after unexpected state
  - **PAUSE REASON values**: `jam`, `broken`, `runout`, `empty`, `backup`, `loading`
  - **Start UI**: Dialog-based tool-to-port assignment before printing (`_IFS_COLORS`)
  - **KAMP**: Adaptive bed mesh at print start (`variable_kamp`)
  - **IFS unlock after boot**: `variable_ifs_unlock_after_boot` for stock screen glitch workaround

### lessWaste _IFS_VARS (Complete Variable List)

```ini
variable_filament_unload_before_cutting: 24
variable_filament_drop_length: 35
variable_filament_unload_after_cutting: 2
variable_filament_unload_speed: 1500
variable_nozzle_cleaning_length: 70
variable_filament_load_speed: 900
variable_filament_home_speed: 700
variable_filament_insert_speed: 2800
variable_filament_tube_length: 1000
variable_filament_catch_length: 5
variable_filament_pressure_length: 1
variable_filament_autoinsert_full_length: 550
variable_tools: [1,2,3,4,5,5,5,5,5,5,5,5,5,5,5,5]  # index=tool, value=port
variable_external: 0
variable_extruder_port: -1
variable_current_tool: -1
variable_extruder_temp: 0
variable_extruder_fan: 0
variable_bed_temp: 0
variable_e_feedrate: 130
variable_e_feedrates: []
variable_consume: 0
variable_kamp: 0
variable_line_purge: 0
variable_backup: 0
variable_types: ['PLA','PLA','PLA','PLA', ...]  # 16 entries
variable_colors: ['000000','000000','000000','000000', ...]  # 16 entries
variable_backup_filament_spent: [0,0,0,0]
variable_start: 0
variable_sbros_trash_speed: 4000
variable_info_dialog: 1
variable_same_filament_purge: 1
variable_ifs_unlock_after_boot: 0
```

### Zmod Slot Renumbering Issue

Zmod has an option to rename slots from 0-indexed (0,1,2,3) to 1-indexed (1,2,3,4). When enabled, the slicer sends T1-T4 instead of T0-T3, causing the `_T` macro to look up `ifs.tools[1]` through `ifs.tools[4]` instead of `ifs.tools[0]` through `ifs.tools[3]`. This is a slicer↔macro configuration mismatch.

**HelixScreen is not affected** — we read the `less_waste_tools` mapping array directly, which is always consistent regardless of slot naming. The off-by-one only matters between the slicer's `T[next_extruder]` output and the Klipper `_T` macro.

---

## 10. Open Questions

1. ~~What does the Moonraker object state actually look like for `zmod_ifs_*` objects?~~ (User's Moonraker API was behind broken reverse proxy — couldn't get state dump)
2. Does `zmod_ifs` expose any Klipper object status attributes (like `get_status()` in the Python module)? Need to check if it implements that method.
3. ~~What does `zmod_color` expose?~~ **Answered**: Source at `github.com/ghzserg/zmod_ff5x` branch `1.6`, file `.shell/zmod_color.py`. Re-reads `Adventurer5M.json` on every G-code command (no cache). When display is OFF (HelixScreen case), reads/writes file directly. Colors use `#` prefix in the file.
4. How does the `file.json` tool mapping interact with multi-color prints? (Slicer outputs T0/T1/etc., mapping resolves to physical ports)

---

## 11. Files Reference

### On Device
| Path | Contents |
|------|----------|
| `/opt/config/mod/.shell/zmod_ifs.py` | IFS Klipper module (symlinked into klippy/extras/) |
| `/opt/config/mod/.shell/zmod_ifs_motion_sensor.py` | Motion sensor wrapper |
| `/opt/config/mod/.shell/zmod_ifs_switch_sensor.py` | Switch sensor wrapper |
| `/opt/config/mod/.shell/zmod_color.py` | Color management module |
| `/usr/prog/config/Adventurer5M.json` | Active channel, filament types/colors |
| `/usr/data/config/mod_data/file.json` | Tool→port mapping |
| `/usr/data/config/mod_data/filament.json` | Per-material profiles |
| `/usr/data/config/mod_data/variables.cfg` | Klipper save_variables |

### Config Structure (ZMOD)
| Path | Purpose |
|------|---------|
| `/opt/config/printer.cfg` | Main config (includes base + mod) |
| `/opt/config/printer.base.cfg` | MCU, steppers, bed, kinematics |
| `/opt/config/mod/ad5x.cfg` | AD5X-specific: sensors, IFS config, save_variables |
| `/opt/config/mod/display_off.cfg` | IFS + sensor macros (tool change, load, unload) |
| `/opt/config/mod/ad5x_config_off.cfg` | SAVE_ZMOD_DATA macro, IFS color management |
| `/opt/config/mod/base_mod.cfg` | PAUSE/RESUME/CANCEL overrides, START_PRINT/END_PRINT |
| `/opt/config/mod/client.cfg` | Client variable macros |
| `/opt/config/mod/motion_sensor.cfg` | Runout detection macros |

---

## 12. Unload Semantics — Empirical Finding

Observed on a real AD5X running native ZMOD: `REMOVE_PRUTOK_IFS PRUTOK=N` runs the **toolhead unload** (heat + retract the filament currently loaded to the toolhead), **not** a per-lane jog. With filament from a *different* slot loaded at the toolhead, `REMOVE_PRUTOK_IFS PRUTOK=3` heated the hotend and unloaded the loaded filament, ignoring the requested port N. On native ZMOD it can error `No filament N in IFS` when the IFS state disagrees with presence.

Consequences:
- A cold per-lane reverse-jog **IS available at the gcode layer** via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (core ZMOD — no heat, no presence guard; §2). This is a thin wrapper over raw serial `F11 C{port}…` with zero heating logic in its handler.
- HelixScreen must still treat `REMOVE_PRUTOK_IFS` / `IFS_REMOVE_PRUTOK` as the heated *toolhead* unload — distinct from `IFS_F11` — and keep the currently-loaded slot unloadable even after runout (#995).
- A true cold per-lane eject is **no longer firmware-blocked**: HelixScreen can call `IFS_F11` directly to recover a snapped chunk stuck in an idle lane's feed path (#996). No hot nozzle is involved in that case, so the cold `IFS_F11 … CHECK=0` retract is the correct tool.

# Filament Management (Developer Guide)

Multi-material system support in HelixScreen: architecture, backend implementations, mock testing, and extension guide.

**User-facing doc**: [docs/user/USER_GUIDE.md](user/USER_GUIDE.md) (filament panel usage, slot operations, troubleshooting)

---

## Architecture Overview

HelixScreen uses a backend abstraction layer to support multiple multi-filament and multi-tool systems through a single UI. The `AmsBackend` interface hides all backend-specific protocols and exposes a uniform API for the UI layer.

```
                         ┌─────────────┐
                         │  AmsState   │  Singleton LVGL subject bridge
                         │ (ams_state) │  Thread-safe subject updates
                         └──────┬──────┘
                                │ owns backends_[] vector
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                  ▼
       Backend 0 (primary)   Backend 1        Backend N
       flat slot subjects    BackendSlot-      BackendSlot-
       (backward compat)     Subjects          Subjects
              │                 │                  │
    ┌─────────▼─────────┐      │                  │
    │     AmsBackend     │  Abstract interface     │
    │  (ams_backend.h)   │  Factory: create() / create_mock()
    └─────────┬──────────┘                         │
     ┌────────┼─────────┬───────────┬──────────────┘
     ▼        ▼         ▼           ▼           ▼           ▼           ▼
  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ ┌──────────┐
  │Happy   │ │  AFC   │ │  ACE   │ │  Tool    │ │ AD5X IFS │ │  CFS   │ │  Mock    │
  │Hare    │ │Backend │ │Backend │ │ Changer  │ │ Backend  │ │Backend │ │ Backend  │
  └────────┘ └────────┘ └────────┘ └──────────┘ └──────────┘ └────────┘ └──────────┘
       │          │          │           │            │           │            │
  Moonraker  Moonraker   REST API   Moonraker   Moonraker  Moonraker    In-memory
  WebSocket  WebSocket   Polling    WebSocket   WebSocket  WebSocket    simulation

                         ┌─────────────┐
                         │  ToolState  │  Singleton: tool abstraction
                         │(tool_state) │  Maps tools ↔ AMS backends
                         └─────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend.h` | Abstract interface with factory methods |
| `include/ams_types.h` | Shared types: `AmsType`, `SlotInfo`, `AmsAction`, `PathTopology`, etc. |
| `include/ams_error.h` | Error types with user-friendly messages |
| `include/ams_state.h` | LVGL subject bridge (singleton) |
| `include/slot_registry.h` | SlotRegistry: single source of truth for per-slot state |
| `src/printer/slot_registry.cpp` | SlotRegistry implementation (name/index mapping, reorganize, tool map) |
| `include/ams_backend_happy_hare.h` | Happy Hare MMU implementation |
| `include/ams_backend_afc.h` | AFC (Armored Turtle / Box Turtle) implementation |
| `include/ams_backend_ace.h` | ACE (Anycubic ACE Pro) implementation |
| `include/ams_backend_toolchanger.h` | Physical tool changer (viesturz/klipper-toolchanger) |
| `include/ams_backend_ad5x_ifs.h` | FlashForge AD5X IFS (Intelligent Filament Switching) |
| `include/ams_backend_cfs.h` | Creality Filament System (K2 series, RS-485) |
| `include/ams_backend_mock.h` | Mock backend for development and testing |
| `src/printer/ams_backend.cpp` | Factory method implementations |
| `include/printer_discovery.h` | Hardware detection from Klipper object list |
| `include/tool_state.h` | Tool abstraction: `ToolInfo`, `ToolState` singleton, tool-backend mapping |
| `include/printer_temperature_state.h` | `ExtruderInfo` struct, multi-extruder dynamic subjects |
| `include/ui_ams_context_menu.h` | Slot context menu (load, unload, edit, spoolman) |
| `include/ui_ams_device_operations_overlay.h` | Device operations overlay (home, recover, bypass, etc.) |

### Data Flow

1. **Discovery**: `PrinterDiscovery::parse_objects()` scans Klipper's `printer.objects.list` for `mmu`, `AFC`, `toolchanger`, `ace`, `AFC_stepper lane*`, `AFC_hub *`, `tool T*`, and `filament_switch_sensor _ifs_port_sensor_*` objects.
2. **Backend Creation**: `AmsState::init_backend_from_hardware()` calls `AmsBackend::create()` with the detected `AmsType` and Moonraker dependencies.
3. **Slot State**: Each backend stores per-slot state in its `SlotRegistry` instance (`slots_`), which provides indexed access, name lookup, and multi-unit reorganization. Moonraker status updates write to the registry under the backend's mutex.
4. **State Sync**: Backend emits events (`STATE_CHANGED`, `SLOT_CHANGED`, etc.) which `AmsState` translates to LVGL subject updates.
5. **UI Binding**: XML widgets bind to subjects (`ams_type`, `ams_action`, `current_slot`, `slots_version`, etc.) for reactive updates.

### SlotRegistry (Per-Slot State)

Each backend owns a `helix::printer::SlotRegistry` instance (`slots_`) that serves as the single source of truth for all per-slot indexed state. Before SlotRegistry, backends maintained parallel vectors (`lane_names_`, `lane_sensors_`, `gate_sensors_`, etc.) that had to be kept in sync manually -- a frequent source of index mismatch bugs.

**What SlotRegistry manages:**
- Slot names and bidirectional name-to-index lookup
- Per-slot sensor states (prep, load, loaded_to_hub, tool_loaded)
- Per-slot error and buffer health
- Per-slot filament weight tracking
- Tool-to-slot mapping
- Multi-unit reorganization (preserving slot data when unit topology changes)

**How backends use it:**

```cpp
// Initialize (once, during startup or first data arrival)
slots_.initialize("AFC Box Turtle", lane_names);   // AFC
slots_.initialize("Happy Hare MMU", gate_count);    // Happy Hare

// Read state
int idx = slots_.index_of("lane3");         // Name -> index
std::string name = slots_.name_of(2);       // Index -> name
const auto* entry = slots_.get(idx);        // Read-only access
auto info = slots_.build_slot_info(idx);    // Build SlotInfo for API

// Write state (under backend mutex)
auto* entry = slots_.get_mut(idx);
entry->sensors.prep = true;
entry->info.color_rgb = 0xFF0000;

// Multi-unit reorganization (AFC multi-unit topology changes)
slots_.reorganize(unit_lane_map);           // Preserves slot data across layout changes
```

**Key design decisions:**
- SlotRegistry does NOT hold a mutex -- the owning backend's mutex protects all access
- `build_slot_info()` constructs a `SlotInfo` snapshot, avoiding shared mutable state
- `reorganize()` takes an ordered vector of unit/lane pairs — caller controls unit ordering
- Slot names remain backend-specific ("lane1" for AFC, "Gate 0" for Happy Hare) -- SlotRegistry is agnostic

### Threading Model

All Moonraker/libhv callbacks arrive on a background thread. Backends update internal state under mutex, then `AmsState` posts subject updates to the LVGL thread via `lv_async_call()`. The UI never directly accesses backend state.

---

## Multi-Backend Architecture

Some printers have multiple filament management systems simultaneously (e.g., a tool changer where each toolhead has its own AFC unit). AmsState supports multiple concurrent backends via a `backends_` vector that replaces the former single `backend_` pointer.

### Backend Storage

```cpp
// AmsState private members
std::vector<std::unique_ptr<AmsBackend>> backends_;       // All backends
std::vector<BackendSlotSubjects> secondary_slot_subjects_; // Per-backend subjects (index 1+)
```

- **Primary backend (index 0)** uses the existing flat `slot_colors_[MAX_SLOTS]` and `slot_statuses_[MAX_SLOTS]` subject arrays. This preserves backward compatibility with all existing XML bindings and single-backend printers.
- **Secondary backends (index 1+)** each get a `BackendSlotSubjects` struct with dynamically allocated `lv_subject_t` vectors:

```cpp
struct BackendSlotSubjects {
    std::vector<lv_subject_t> colors;
    std::vector<lv_subject_t> statuses;
    int slot_count = 0;
    void init(int count);   // Allocate and init subjects
    void deinit();          // Deinit subjects
};
```

### Discovery of Multiple Systems

`PrinterDiscovery::parse_objects()` collects all detected AMS/filament systems into a `detected_ams_systems_` vector of `DetectedAmsSystem` structs:

```cpp
struct DetectedAmsSystem {
    AmsType type = AmsType::NONE;
    std::string name;  // "Happy Hare", "AFC", "Tool Changer"
};
```

A printer with both a tool changer and an AFC unit will have two entries. The `init_backends_from_hardware()` method iterates this list and creates a backend for each detected system.

### Backend Selection

Two new subjects track backend selection:

| Subject | Type | Description |
|---------|------|-------------|
| `backend_count_` | int | Number of registered backends |
| `active_backend_` | int | Index of the currently selected backend |

The AMS panel UI shows a backend selector when `backend_count > 1`, allowing users to switch between systems. API:

- `active_backend_index()` -- returns the currently selected backend index
- `set_active_backend(int)` -- switches the active backend (bounds-checked)

### Per-Backend Event Routing

When backends are added via `add_backend()`, each backend's event callback captures its backend index at registration time:

```
Backend 0 emits STATE_CHANGED  -->  on_backend_event(0, "STATE_CHANGED", ...)
Backend 1 emits SLOT_CHANGED   -->  on_backend_event(1, "SLOT_CHANGED", ...)
```

The `on_backend_event()` handler routes to `sync_backend(int)` or `update_slot_for_backend(int, int)` which update the correct set of subjects. All subject updates are posted via `ui_async_call()` for thread safety.

### Per-Backend Subject Access

Two-argument overloads of `get_slot_color_subject()` and `get_slot_status_subject()` route to the correct subject storage:

```cpp
// Backend 0: flat arrays (backward compat)
lv_subject_t* get_slot_color_subject(0, slot_index);  // -> slot_colors_[slot_index]

// Backend 1+: per-backend storage
lv_subject_t* get_slot_color_subject(1, slot_index);  // -> secondary_slot_subjects_[0].colors[slot_index]
```

### Tool-Backend Integration

The `ToolState` singleton (see `tool_state.h`) maps tools to specific AMS backends via two fields on `ToolInfo`:

```cpp
struct ToolInfo {
    int backend_index = -1;  // Which AMS backend feeds this tool (-1 = direct drive)
    int backend_slot = -1;   // Fixed slot in that backend (-1 = any/dynamic)
    // ... other fields
};
```

- `backend_index = -1` means the tool uses direct-drive filament (no AMS).
- `backend_index >= 0` maps the tool to a specific AMS backend. For example, on a dual-toolhead printer where each head has its own AFC unit, T0 might map to backend 0 and T1 to backend 1.
- `backend_slot` pins the tool to a specific slot within that backend, or `-1` for dynamic slot selection (e.g., Happy Hare tool-to-gate mapping).

`ToolState` and `AmsState` coordinate through `PrinterDiscovery`: tools are discovered from `tool T*` Klipper objects, and the mapping between tools and AMS backends is established during `init_backends_from_hardware()`.

---

## Persistence of slot metadata

HelixScreen writes user-edited slot metadata (brand, spool name, Spoolman
link, weights, color/material) to the Moonraker `lane_data` namespace,
following the AFC-originated convention. This is the same namespace
OrcaSlicer 2.3.2+ reads for filament sync, so user edits automatically
flow to the slicer on Moonraker-based printers. The flow is one-directional:
HelixScreen writes, OrcaSlicer reads (it never writes `lane_data` back), so
"round-trip" here means the user's edit reaching the slicer's filament
panel — not a slicer-to-printer write.

- **Wire-format spec (public):** [`../specs/filament_slots.md`](../specs/filament_slots.md)
- **Implementation notes (internal):** [`FILAMENT_SLOT_METADATA.md`](FILAMENT_SLOT_METADATA.md)

### OrcaSlicer compatibility — by backend

All HelixScreen-managed AMS backends write the AFC-standard `lane_data`
record on edit, so every one of them round-trips to OrcaSlicer with no
additional configuration. **Verified against OrcaSlicer upstream/main
(post-2.4.0-beta nightly)**, source `MoonrakerPrinterAgent.cpp`
`fetch_moonraker_filament_data()`.

| Backend | Writer | How OrcaSlicer picks it up |
|---------|--------|----------------------------|
| AD5X IFS | HelixScreen (`FilamentSlotOverrideStore`) | `lane_data` namespace |
| Snapmaker U1 | HelixScreen (`FilamentSlotOverrideStore`) | `lane_data` namespace |
| ACE (Anycubic ACE Pro) | HelixScreen (`FilamentSlotOverrideStore`) | `lane_data` namespace |
| CFS (Creality K2) | HelixScreen (`FilamentSlotOverrideStore`) | `lane_data` namespace |
| AFC / Box Turtle | AFC's own Klipper plugin | `lane_data` namespace (AFC is the originator) |
| Happy Hare | Happy Hare's own Klipper plugin (`components/mmu_server.py` `push_lane_data`) | `lane_data` namespace — Orca prefers it over the live `mmu` object |
| Tool Changer | (not applicable — no per-slot metadata) | N/A |

IFS, Snapmaker, ACE, and CFS share the `FilamentSlotOverrideStore`
infrastructure and publish to `lane_data`; AFC and Happy Hare each write
`lane_data` via their own Klipper plugins. **HelixScreen never writes
`lane_data` for the AFC or Happy Hare backends** — those plugins own their
records, and HelixScreen's AFC/HH backends route user edits through G-code
(`SET_COLOR`/`SET_MATERIAL`, `MMU_GATE_MAP`) only, so there is no clobber risk.
(Earlier docs said HH reached Orca solely via the live `mmu` Klipper object.
That is outdated: HH's `push_lane_data` now writes the namespace directly and
Orca prefers it; the `mmu` object is the fallback.)

#### Schema lineage and what Orca actually matches on

- **AFC pioneered the base schema** — keys `color` / `material` / `bed_temp` /
  `nozzle_temp` / `scan_time` / `td` / `lane` / `spool_id`, DB key 1-based
  (`lane1`…), inner `lane` field 0-based. AFC emits **no** vendor field.
- **Happy Hare extended it** with `vendor_name`, `name`, and `filament_id`
  (`push_lane_data`). HH's `filament_id` is a **Spoolman** DB id, not an
  OrcaSlicer preset id.
- **OrcaSlicer reads only `lane` / `material` / `color` / `bed_temp` /
  `nozzle_temp`**, and matches a lane to a filament preset **by the `material`
  type string alone** (`filament_id_by_type`, falling back to generic
  OrcaFilamentLibrary ids like `OGFL99`). It ignores `vendor`, `vendor_name`,
  and `filament_id` today, and never writes `lane_data` back. So brand has no
  effect on the slicer's preset pick — "Generic PLA" and "Elegoo PLA+" both
  resolve to a generic PLA preset. Emit canonical material strings (`PLA`,
  `PETG`, `ABS`…); marketing names won't match.

#### Forward-compat aliases (`vendor_name` / `name`)

HelixScreen's writer (`to_lane_data_record()` in
`filament_slot_override_store.cpp`) emits **both** key spellings: `vendor`
(AFC-style base) **and** `vendor_name` (HH extension), plus `spool_name` **and**
`name`. It's unsettled which spelling Orca will consume when it eventually adds
vendor-aware matching, so emitting both is a zero-cost hedge — Orca ignores
unknown keys. The reader tolerantly falls back to the HH aliases. **Do not add
a HelixScreen-side `filament_id` resolver:** Orca reads the field from nowhere,
there is no deterministic (vendor, material) → Orca `setting_id` catalog (the
ids number in the hundreds and churn across releases), and we do not ship a
forked OrcaSlicer that could add the read path.

---

## UI Panels

### AMS Panel (`ui_panel_ams`)

The detail panel showing slots, path visualization, hub sensors, and the currently loaded filament for a single backend. Opened as an overlay from the Filament nav panel or from the AMS Overview Panel.

Key features:
- Slot grid with overlap layout for >4 slots (shared via `ui_ams_slot_layout.h`)
- Path canvas showing filament routing from slots through hub to toolhead
- Backend selector (shown when `backend_count > 1`)
- Unit scoping: can display a subset of slots for a single unit within a multi-unit backend

### AMS Overview Panel (`ui_panel_ams_overview`)

Grid of unit cards showing all units across the system. Each card is a miniature visualization of the unit's slots. Clicking a card transitions inline to a detail view of that unit's slots.

Key files:
| File | Purpose |
|------|---------|
| `include/ui_panel_ams_overview.h` | Class with detail view state |
| `src/ui/ui_panel_ams_overview.cpp` | Card creation, inline detail view, slot layout |
| `ui_xml/ams_overview_panel.xml` | Two-column layout: cards/detail left, loaded info right |
| `ui_xml/ams_unit_card.xml` | Mini unit card with slot bars and hub sensor dot |

**Current scope**: The overview panel queries `get_backend(0)` and displays all units from that single backend's `AmsSystemInfo`. This covers the common case of a single multi-unit AMS system (e.g., AFC with multiple Box Turtle units).

**Future: multi-backend aggregation**: When multiple backends are active simultaneously (e.g., an AFC system on one toolhead + a Happy Hare on another), the overview panel should iterate all backends via `AmsState::get_backend(i)` for `i` in `0..backend_count` and aggregate their units into the card grid. The per-backend slot subject storage (`secondary_slot_subjects_`) and event routing already support this — the UI aggregation is the remaining integration point.

### Error State Visualization

Per-slot error indicators and per-unit error badges, driven by `SlotInfo.error` and `SlotInfo.buffer_health` from the backend layer. See `docs/devel/plans/2026-02-15-error-state-visualization-design.md` for full design.

**Data model** (`ams_types.h`):
- `SlotError` — message + severity (INFO/WARNING/ERROR), `std::optional` on `SlotInfo`
- `BufferHealth` — AFC buffer fault proximity data, `std::optional` on `SlotInfo`
- `AmsUnit::has_any_error()` — rolls up per-slot errors for overview badge

**Detail view** (`ui_ams_slot.cpp`):
- 14px error badge at top-right of spool (red for ERROR, yellow for WARNING)
- 8px buffer health dot at bottom-center (green/yellow/red based on fault proximity)
- Both pulled from `SlotInfo` during refresh (same pattern as material/tool badge)

**Overview view** (`ui_panel_ams_overview.cpp`):
- 12px error badge at top-right of unit card (worst severity across slots)
- Mini-bar status lines colored by error severity

**Backend integration**:
- AFC: per-lane error from `status` field + buffer health from `AFC_buffer` objects
- Happy Hare: system-level error mapped to `current_slot` via `reason_for_pause`
- Mock: `set_slot_error()` / `set_slot_buffer_health()` + pre-populated errors in AFC mode

---

## Supported Backends

### AmsType Enum

```cpp
enum class AmsType {
    NONE = 0,         // No AMS detected
    HAPPY_HARE = 1,   // Happy Hare MMU (mmu object in Moonraker)
    AFC = 2,          // AFC-Klipper-Add-On (AFC object, lane_data database)
    ACE = 3,          // AnyCubic ACE Pro (ValgACE/BunnyACE/DuckACE Klipper drivers)
    TOOL_CHANGER = 4, // Physical tool changer (viesturz/klipper-toolchanger)
    AD5X_IFS = 5,     // FlashForge AD5X IFS (Intelligent Filament Switching)
    CFS = 6,          // Creality Filament System (K2 series, RS-485)
    SNAPMAKER = 7,    // Snapmaker U1 SnapSwap toolchanger
    QIDI_BOX = 8      // QIDI Box (PLUS4 / Q2 / MAX4, hub-style, 4 slots chainable to 16) — STUB
};
```

Helper functions: `is_tool_changer()` and `is_filament_system()` distinguish between the two categories.

---

## Happy Hare (MMU)

Happy Hare is a Klipper add-on for ERCF, Tradrack, and other selector-based multi-filament systems.

### Detection

Klipper object `mmu` in `printer.objects.list` sets `AmsType::HAPPY_HARE`.

### Moonraker Variables

| Variable | Type | Description |
|----------|------|-------------|
| `printer.mmu.gate` | int | Current gate (-1=none, -2=bypass) |
| `printer.mmu.tool` | int | Current tool number |
| `printer.mmu.filament` | string | "Loaded" or "Unloaded" |
| `printer.mmu.action` | string | "Idle", "Loading", "Unloading", "Forming Tip", etc. |
| `printer.mmu.gate_status` | int[] | Per-gate: -1=unknown, 0=empty, 1=available, 2=from_buffer |
| `printer.mmu.gate_color_rgb` | int[] | Per-gate RGB colors (0xRRGGBB) |
| `printer.mmu.gate_material` | string[] | Per-gate material names |
| `printer.mmu.filament_pos` | int | 0-8 filament position for path visualization |

### G-code Commands

| Command | Action |
|---------|--------|
| `MMU_LOAD GATE={n}` | Load filament from gate |
| `MMU_UNLOAD` | Unload current filament |
| `MMU_SELECT GATE={n}` | Select gate without loading |
| `T{n}` | Tool change (unload + load) |
| `MMU_HOME` | Home the selector (reset) |
| `MMU_RECOVER` | Attempt error recovery |
| `MMU_TTG_MAP TOOL={n} GATE={g}` | Set tool-to-gate mapping |
| `MMU_SELECT_BYPASS` | Select bypass position |

### Path Topology

`PathTopology::LINEAR` -- Selector picks one input from multiple gates. Filament path: `SPOOL -> PREP -> LANE -> HUB (selector) -> OUTPUT (bowden) -> TOOLHEAD -> NOZZLE`.

Happy Hare's `filament_pos` (0-8) maps to `PathSegment` via `path_segment_from_happy_hare_pos()`.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | Yes | Read-only (configured in `mmu_vars.cfg`) |
| Tool Mapping | Yes | Yes (via `MMU_TTG_MAP`) |
| Bypass Mode | Yes | Yes (selector position -2) |
| Spoolman | Yes | -- |
| Auto-Heat on Load | No | UI manages preheat |
| Dryer | No | -- |

### Reset vs Recover

- **Reset** (`reset()`) sends `MMU_HOME` to home the selector. Used for general state reset.
- **Recover** (`recover()`) sends `MMU_RECOVER` to attempt error recovery without full re-homing.

---

## AFC (Armored Turtle / Box Turtle)

AFC-Klipper-Add-On supports multiple hardware types (Box Turtle, OpenAMS) with different topologies. A single AFC installation can mix hardware types — e.g., a Box Turtle feeding 4 toolheads alongside two OpenAMS units each feeding 1 toolhead.

### Hardware Types and Klipper Objects

AFC hardware types register as different Klipper object prefixes:

| Hardware | Klipper Object Prefix | Lane Object | Hub Object | Topology |
|----------|----------------------|-------------|------------|----------|
| Box Turtle | `AFC_BoxTurtle {name}` | `AFC_stepper lane{N}` | `AFC_hub {name}` or none | HUB (standard) or PARALLEL (toolchanger) |
| OpenAMS | `AFC_OpenAMS {name}` | `AFC_lane lane{N}` | `AFC_hub Hub_{N}` | HUB (always) |
| Toolchanger | `AFC_Toolchanger {name}` | — | — | Container only |

**Critical: OpenAMS uses `AFC_lane`, not `AFC_stepper`.** Both have the same JSON schema and are parsed through the same `parse_afc_stepper()` function. We subscribe to both object types.

### Unit Object Structure (Real Production Data)

Each unit-type object provides lane/extruder/hub/buffer membership. This data comes from Klipper, not from individual lane queries.

**Box Turtle in toolchanger mode** (`AFC_BoxTurtle Turtle_1`):
```json
{
    "lanes": ["lane0", "lane1", "lane2", "lane3"],
    "extruders": ["extruder", "extruder1", "extruder2", "extruder3"],
    "hubs": [],
    "buffers": ["TN", "TN1", "TN2", "TN3"]
}
```
- 4 extruders → PARALLEL topology (each lane feeds its own toolhead)
- No hubs (lanes use `hub: "direct_load"` — direct connection to extruder)
- TurtleNeck buffers per lane

**OpenAMS** (`AFC_OpenAMS AMS_1`):
```json
{
    "lanes": ["lane4", "lane5", "lane6", "lane7"],
    "extruders": ["extruder4"],
    "hubs": ["Hub_1", "Hub_2", "Hub_3", "Hub_4"],
    "buffers": []
}
```
- 1 extruder → HUB topology (all 4 lanes converge to 1 toolhead)
- Per-lane hubs: each lane has its own hub (Hub_1 for lane4, Hub_2 for lane5, etc.)
- Hub names do NOT match unit names (Hub_1 ≠ AMS_1)
- No buffers (no TurtleNeck needed)

### Topology Determination

AFC topology is inferred from the extruder count per unit:
- **1 extruder** → `PathTopology::HUB` (all lanes merge to one toolhead)
- **N extruders (N == lane count)** → `PathTopology::PARALLEL` (1:1 lane-to-tool mapping)

This is stored per-unit in `unit_topologies_[]` and queried via `get_unit_topology(unit_index)`.

### The `map` Field Problem

AFC assigns each lane a virtual tool number via the `map` field (e.g., `"T4"`). **For HUB units, AFC gives each lane a unique map value even though all lanes physically feed the same extruder.**

Real production data from a 6-toolhead mixed system:

| Lane | Unit | Hub | Extruder | map | Physical Tool |
|------|------|-----|----------|-----|---------------|
| lane0 | Turtle_1 | direct_load | extruder | T0 | T0 |
| lane1 | Turtle_1 | direct_load | extruder1 | T1 | T1 |
| lane2 | Turtle_1 | direct_load | extruder2 | T2 | T2 |
| lane3 | Turtle_1 | direct_load | extruder3 | T3 | T3 |
| lane4 | AMS_1 | Hub_1 | extruder4 | T4 | T4 |
| lane5 | AMS_1 | Hub_2 | extruder4 | T5 | **T4** (same physical nozzle) |
| lane6 | AMS_1 | Hub_3 | extruder4 | T6 | **T4** (same physical nozzle) |
| lane7 | AMS_1 | Hub_4 | extruder4 | T7 | **T4** (same physical nozzle) |
| lane8 | AMS_2 | Hub_5 | extruder5 | T8 | T5 |
| lane9 | AMS_2 | Hub_6 | extruder5 | T9 | **T5** (same physical nozzle) |
| lane10 | AMS_2 | Hub_7 | extruder5 | T10 | **T5** (same physical nozzle) |
| lane11 | AMS_2 | Hub_8 | extruder5 | T11 | **T5** (same physical nozzle) |

**The `map` field represents virtual tool numbers for AFC's internal routing, not physical toolheads.** The UI must use topology to determine physical tool count for drawing nozzles:
- PARALLEL: `tool_count = max_tool - min_tool + 1` (each map value = different nozzle)
- HUB: `tool_count = 1` (all map values = same nozzle)

### Hub Sensor Propagation

Standard Box Turtle: hub name matches unit name (e.g., hub "Turtle_1" for unit "Turtle_1"), so `hub_name == unit.name` works.

OpenAMS: hub names are per-lane (Hub_1, Hub_2, ..., Hub_8) and do NOT match the unit name (AMS_1, AMS_2). Hub sensor state must be propagated by looking up which unit owns the hub via the `unit_infos_` hub membership lists.

The code uses a two-strategy approach:
1. Check `unit_infos_[].hubs` to find the parent unit (handles OpenAMS)
2. Fallback: direct `hub_name == unit.name` match (handles standard Box Turtle)

If ANY hub in a unit is triggered, `unit.hub_sensor_triggered = true`.

### AFC Lane Status Values

Status values observed in production and their mapping to `SlotStatus`:

| AFC Status | `tool_loaded` | Meaning | SlotStatus |
|------------|---------------|---------|------------|
| `"Tooled"` | any | Actively loaded in toolhead (OpenAMS) | LOADED |
| `"Loaded"` | true | Filament loaded to toolhead | LOADED |
| `"Loaded"` | false | Filament loaded to hub (not toolhead) | AVAILABLE |
| `"Ready"` | false | Filament present, sensors triggered | AVAILABLE |
| `"None"` | false | No filament, no sensors | EMPTY |
| `"Error"` | any | Lane error | AVAILABLE + SlotError |
| `""` (empty) | false | No data yet | EMPTY |

**Critical**: AFC's `"Loaded"` status means hub-loaded, NOT toolhead-loaded. The `tool_loaded` boolean is the authoritative indicator of toolhead presence. Only `tool_loaded: true` or `status: "Tooled"` maps to `SlotStatus::LOADED`. The `"Loaded"` status string alone (with `tool_loaded: false`) maps to `AVAILABLE`.

### Other AFC Lane Fields

Fields present in production `AFC_lane` data but not in `AFC_stepper`:
- `buffer: null` and `buffer_status: null` (OpenAMS has no buffers)
- `dist_hub: 60` (OpenAMS, short distance) vs `1940-2230` (Box Turtle, long bowden)
- `td1_td`, `td1_color`, `td1_scan_time` — TD1 filament tag detection sensor data (not currently used by HelixScreen)

### Detection

Klipper object `AFC` in `printer.objects.list` sets `AmsType::AFC`. Lane names come from `AFC_stepper lane*` and `AFC_lane lane*` objects, hub names from `AFC_hub *` objects. Unit-type objects (`AFC_BoxTurtle`, `AFC_OpenAMS`) provide the lane/extruder/hub/buffer membership that determines per-unit topology.

### Data Sources

AFC state comes from multiple Klipper objects:

**Per-lane state** (`AFC_stepper lane{N}` or `AFC_lane lane{N}`):

| Field | Type | Description |
|-------|------|-------------|
| `prep` | bool | Prep sensor triggered |
| `load` | bool | Load sensor triggered |
| `loaded_to_hub` | bool | Filament reached hub |
| `tool_loaded` | bool | Filament loaded to toolhead |
| `status` | string | "Loaded", "Tooled", "Ready", "None", "Error" |
| `color` | string | Filament color hex (`#RRGGBB`) |
| `material` | string | Material type from Spoolman |
| `spool_id` | int | Spoolman spool ID |
| `weight` | float | Remaining weight in grams |
| `buffer_status` | string | Buffer state (e.g., "Advancing") |
| `filament_status` | string | Readiness (e.g., "Ready", "Not Ready") |
| `dist_hub` | float | Distance to hub in mm |

**Hub state** (`AFC_hub {name}`):

| Field | Type | Description |
|-------|------|-------------|
| `state` | bool | Hub sensor triggered |
| `afc_bowden_length` | float | Bowden tube length from hub to toolhead (mm) |

**Extruder state** (`AFC_extruder extruder`):

| Field | Type | Description |
|-------|------|-------------|
| `tool_start_status` | bool | Toolhead entry sensor |
| `tool_end_status` | bool | Toolhead exit/nozzle sensor |
| `lane_loaded` | string | Currently loaded lane name |

**Global state** (`AFC`):

| Field | Type | Description |
|-------|------|-------------|
| `current_lane` | string | Active lane name (or null) |
| `current_state` | string | "Idle", "Loading", "Unloading", etc. |
| `error_state` | bool | AFC error flag |
| `lanes[]` | string[] | List of lane names |
| `quiet_mode` | bool | Quiet mode state |
| `led_state` | bool | LED strip on/off |

**Moonraker database** (AFC namespace, `lane_data` key -- v1.0.32+):

```json
{
  "lane1": {"color": "FF0000", "material": "PLA", "loaded": false},
  "lane2": {"color": "00FF00", "material": "PETG", "loaded": true}
}
```

### G-code Commands

| Command | Action |
|---------|--------|
| `AFC_LOAD LANE={name}` | Load filament from lane |
| `AFC_UNLOAD` | Unload current filament |
| `AFC_CUT LANE={name}` | Cut filament (if cutter installed) |
| `AFC_HOME` | Home the AFC system (reset) |
| `AFC_RESET` | Reset from error state (recover) |
| `T{n}` | Tool change (unload + load) |
| `SET_MAP LANE={name} MAP=T{n}` | Set lane-to-tool mapping |
| `SET_BOWDEN_LENGTH UNIT={unit_name} LENGTH={mm}` | Set bowden tube length for a unit |
| `SET_RUNOUT LANE={name} RUNOUT={backup_lane}` | Set endless spool backup |
| `RESET_AFC_MAPPING RUNOUT=no` | Reset tool mappings only |
| `AFC_CALIBRATION` | Run calibration wizard |
| `AFC_PARK` | Park the AFC system |
| `AFC_BRUSH` | Run brush cleaning sequence |
| `AFC_RESET_MOTOR_TIME` | Reset motor run-time counter |
| `TURN_ON_AFC_LED` / `TURN_OFF_AFC_LED` | Toggle LED strip |
| `AFC_QUIET_MODE` | Toggle quiet mode |

### Path Topology

`PathTopology::HUB` -- Multiple lanes merge into a common hub/merger. Sensor-based position inference:

```
No sensors            -> SPOOL (filament present but not advanced)
prep only             -> HUB (past prep, approaching hub)
prep + hub            -> TOOLHEAD (past hub, approaching toolhead)
prep + hub + toolhead -> NOZZLE (fully loaded)
```

See `path_segment_from_afc_sensors()` in `ams_types.h`.

### AFC-Specific Features

#### Hub Bowden Length

The bowden tube length from hub to toolhead is read from `AFC_hub.afc_bowden_length` and exposed as a slider in the device actions UI. Adjustable via `SET_BOWDEN_LENGTH LENGTH={mm}` G-code.

#### Per-Lane Stepper Fields

Each `AFC_stepper` object provides sensor states (`prep`, `load`, `loaded_to_hub`), buffer state (`buffer_status`), filament readiness (`filament_status`), and distance to hub (`dist_hub`). These are cached in the `LaneSensors` struct per lane (up to 16 lanes).

#### Buffer Objects

AFC tracks buffer state per lane. The `buffer_status` field indicates the current buffer operation (e.g., "Advancing"). Buffer names are discovered from the Klipper object list.

#### Global State

The `AFC` Klipper object provides global state: `current_lane`, `current_state`, `error_state`, `quiet_mode`, and `led_state`. These drive the UI status display and device action toggles.

#### Maintenance Mode

The device operations overlay exposes AFC maintenance actions:

| Action | G-code | Description |
|--------|--------|-------------|
| Test All Lanes | `AFC_TEST_LANES` | Run test sequence on all lanes |
| Change Blade | `AFC_CHANGE_BLADE` | Initiate blade change procedure |
| Park | `AFC_PARK` | Park the AFC system |
| Clean Brush | `AFC_BRUSH` | Run nozzle cleaning brush cycle |
| Reset Motor Timer | `AFC_RESET_MOTOR_TIME` | Reset motor run-time counter |

#### LED Toggle

The LED toggle sends `TURN_ON_AFC_LED` or `TURN_OFF_AFC_LED` based on the current `afc_led_state_`. The button label and icon dynamically reflect the current state.

#### Quiet Mode

Quiet mode reduces motor noise at the cost of speed. Toggled via `AFC_QUIET_MODE` G-code. The current state is tracked via `afc_quiet_mode_` from the `AFC.quiet_mode` printer object field.

#### Per-Lane Reset

AFC supports resetting individual lanes via `reset_lane(slot_index)`, which sends `AFC_RESET LANE={name}`. This resets a single lane to a known good state without affecting others.

#### Reset vs Recover

- **Reset** (`reset()`) sends `AFC_HOME` to home the entire AFC system.
- **Recover** (`recover()`) sends `AFC_RESET` to recover from error state. Less disruptive than a full home.
- **Per-lane reset** (`reset_lane()`) targets a single lane with `AFC_RESET LANE={name}`.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | Yes | Yes (per-slot via `SET_RUNOUT`) |
| Tool Mapping | Yes | Yes (via `SET_MAP`) |
| Bypass Mode | Yes | Hardware sensor (auto-detect on Box Turtle) |
| Spoolman | Yes | -- |
| Auto-Heat on Load | Yes | AFC uses `default_material_temps` from config |
| Dryer | No | -- |
| Device Actions | Yes | Calibration, Speed, Maintenance, LED/Modes |

### AFC Version Differences

| Feature | v1.0.0 | v1.0.32+ |
|---------|--------|----------|
| `AFC_stepper lane*` objects | Full sensor data | Same |
| `lane_data` in Moonraker DB | Not available | Available (richer data) |
| TD1 sensor support | No | Yes |
| Auto-level during home | No | Yes |

The backend detects the installed version by querying the `afc-install` database namespace and sets `has_lane_data_db_` for v1.0.32+. The version check uses `version_at_least()`.

---

## ACE (Anycubic ACE Pro)

The ACE backend supports the Anycubic ACE Pro multi-material hub. The same hardware
shows up behind **several different software stacks**, and they expose *different*
Klipper/Moonraker interfaces. "ACE support" is therefore not one integration — it is
whichever of these the printer is running:

| # | Stack | Klipper / Moonraker surface | Transport | Audience | Backend status |
|---|-------|-----------------------------|-----------|----------|----------------|
| 1 | **Native Anycubic GoKlipper (via Rinkhals)** | `filament_hub` printer object (config `[ace]`) | WebSocket query/subscribe | **Primary real user base** — stock Kobra 3 / 3 V2 / 3 Max / S1 / S1 Max (Combo) flashed with [Rinkhals](https://github.com/jbatonnet/Rinkhals) | ✅ Handled (parses `filament_hub`) |
| 2 | **Community ValgACE / BunnyACE / DuckACE** | `ace` printer object + `ace_status.py` | `/server/ace/*` REST bridge | ACE Pro bolted onto a **non-Anycubic DIY printer** (niche; DuckACE abandoned) | ✅ Handled (REST fallback) |
| 3 | **Mainline-Python Kobra-S1 fork** (`github.com/Kobra-S1/klipper-kobra-s1`) | custom `[ace]` extra + `[ace_status]` Moonraker component | **unconfirmed** (`ace_status.py` JSON/REST) | KS1 users replacing KobraOS with mainline Klipper (often on an external Pi) | ❓ **Unverified** — status surface not yet inspected |

**How to think about the three:**

- **Path 1 (native)** is what almost every actual ACE user runs — it ships inside
  Anycubic's own GoKlipper firmware and is surfaced when the printer is reflashed with
  Rinkhals. This is the path the backend is built around.
- **Path 2 (community)** is ACE-on-a-DIY-rig: ValgACE (active), plus the BunnyACE/DuckACE
  forks (DuckACE abandoned). Integrates through Moonraker macros/endpoints rather than a
  native Klipper object.
- **Path 3 (KS1 fork)** is newly observed in real logs (2026-06-24) and **not yet
  validated against our backend.** It is a *full Klipper firmware fork* for the Kobra S1
  — related to the Path 2 driver concept (it too ships an `ace_status.py`) but wrapped in
  KS1-specific cutter/purge/toolchange macros. Whether its `[ace_status]` surface matches
  Path 1's `filament_hub`, Path 2's `ace`/REST, or neither is an **open question**.
  Control gcode (`ACE_CHANGE_TOOL`, `ACE_ENABLE/DISABLE_FEED_ASSIST`) does match what the
  backend already sends. Full teardown:
  [`printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md`](printer-research/ANYCUBIC_ACE_KOBRA_S1_LOG_ANALYSIS.md).

> The sections below (`filament_hub` schema, REST endpoints, etc.) document Paths 1 and 2,
> which the backend handles today. Path 3's status schema is still TBD — see the linked
> log-analysis doc for the open items needed to confirm or extend coverage.

### History

The ACE backend was originally written **blind for ValgACE** (keying on a Klipper object literally named `ace`) and never matched a real Anycubic ACE hub — so Combo printers on Rinkhals got no AMS backend detected at all. Fixed **2026-06-13** to detect `filament_hub` first. The native object name was confirmed in Anycubic GoKlipper `extras_ace.go` and Rinkhals `mmu_ace.py`. The native `ACE_*` G-code verbs turned out to be exactly what the backend was already sending (ValgACE mirrored them), so the fix was a detection + status-parsing change, not a command-dialect rewrite.

### Detection

ACE is detected in two ways:

1. **Object list detection**: `filament_hub` (native Anycubic/Rinkhals) **or** `ace` (community drivers) in `printer.objects.list`.
2. **REST probe fallback**: A probe to `/server/ace/info` via `AmsState::probe_ace()` catches **community** setups where the object list is unavailable. (The native path never needs this — `filament_hub` is always in `objects.list`.)

### Native `filament_hub` Status Schema

The native GoKlipper `filament_hub.get_status()` is **flat and single-hub** (one hub, 4 slots). Multi-unit "Combo" configurations (8 slots) are a Rinkhals-layer abstraction stacked above this single-hub GoKlipper object.

| Field | Type | Meaning |
|-------|------|---------|
| `status` | string | Overall hub status |
| `dryer.status` | string | Dryer running/idle |
| `dryer.target_temp` | int | Dryer target temperature |
| `dryer.duration` | int | Configured drying duration |
| `dryer.remain_time` | int | Remaining drying time |
| `temp` | int | Hub temperature |
| `slots[]` | array | Per-slot state (4 entries) |
| `slots[].index` | int | Slot index |
| `slots[].status` | string | `empty` / `ready` / `preload` / `running` / `runout` |
| `slots[].sku` | string | Filament SKU |
| `slots[].type` | string | Filament material type |
| `slots[].color` | `[r, g, b]` | Slot color |
| `current_filament` | string | Loaded slot as `"<unitId>-<localIndex>"` (e.g. `"0-2"`); empty/absent = nothing loaded |

### G-code Commands (native `ACE_*`)

These are the real native verbs from GoKlipper `extras_ace.go` — the backend drives the native path with exactly these:

| Command | Action |
|---------|--------|
| `ACE_CHANGE_TOOL TOOL={n}` | Load slot (or `-1` to unload) |
| `ACE_FEED INDEX={i} LENGTH={mm} SPEED={s}` | Feed filament from a slot |
| `ACE_RETRACT INDEX={i} LENGTH={mm} SPEED={s}` | Retract filament to a slot |
| `ACE_ENABLE_FEED_ASSIST INDEX={i}` | Enable feed assist on a slot |
| `ACE_DISABLE_FEED_ASSIST INDEX={i}` | Disable feed assist on a slot |
| `ACE_START_DRYING TEMP={t} DURATION={m}` | Start drying |
| `ACE_STOP_DRYING` | Stop drying |

> Note: `ACE_RECOVER` and `ACE_RESET` are **not** native GoKlipper commands — do not send them on the native path.

### REST Endpoints (community fallback only)

These belong to ValgACE's Moonraker component (`ace_status.py`) and are used **only** on the community fallback path; the native Rinkhals deployment never uses them. BunnyACE/DuckACE users must install ValgACE's `ace_status.py` separately to get this bridge.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/server/ace/info` | GET | System information (model, version, slot count) |
| `/server/ace/status` | GET | Current state (dryer, loaded slot, action) |
| `/server/ace/slots` | GET | Slot information (colors, materials, status) |

### Threading

- **Native path (`filament_hub`):** WebSocket query + subscription. State is held under `mutex_`; updates arriving on the WebSocket background thread are deferred to the main thread via `token.defer(...)` (L081-safe — never mutate UI state directly from the WS callback).
- **Community fallback path (`ace`):** a background polling thread runs at ~500ms intervals when the backend is active, caching state under mutex protection.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | No | -- |
| Tool Mapping | No | Fixed 1:1 mapping |
| Bypass Mode | No | -- |
| Spoolman | No | -- |
| Auto-Heat on Load | No | -- |
| Dryer | Yes | Built-in hardware dryer |

### Dryer Control

ACE is the primary backend with integrated dryer support. The `DryerInfo` struct provides:

- Current/target temperature
- Duration and remaining time
- Fan speed control
- Hardware capability limits (min/max temp, max duration)

Drying presets are derived from the filament database via `get_default_drying_presets()`.

On the native path, live dryer state (status, target temp, duration, remaining time) is parsed directly from `filament_hub.dryer`.

---

## Tool Changer (viesturz/klipper-toolchanger)

Physical tool changers have multiple complete toolheads that are swapped on the carriage, fundamentally different from filament-switching systems.

### Detection

Klipper object `toolchanger` in `printer.objects.list` sets `AmsType::TOOL_CHANGER`. Individual tool names come from `tool T*` objects (e.g., `tool T0`, `tool T1`).

### Key Differences from Filament Systems

- Each "slot" is a complete toolhead with its own extruder
- No hub/selector -- path topology is `PARALLEL`
- "Loading" means mounting the tool to the carriage
- No bypass mode (each tool IS the path)
- Tool mapping is fixed (tools ARE slots)

### Klipper Objects

**Global** (`toolchanger`):

| Variable | Type | Description |
|----------|------|-------------|
| `status` | string | "ready", "changing", "error", "uninitialized" |
| `tool` | string | Current tool name ("T0") or null |
| `tool_number` | int | Current tool number (-1 if none) |
| `tool_numbers` | int[] | All tool numbers [0, 1, 2] |
| `tool_names` | string[] | All tool names ["T0", "T1", "T2"] |

**Per-tool** (`tool T{n}`):

| Variable | Type | Description |
|----------|------|-------------|
| `active` | bool | Is this tool selected? |
| `mounted` | bool | Is this tool mounted on carriage? |
| `gcode_x_offset` | float | X offset |
| `gcode_y_offset` | float | Y offset |
| `gcode_z_offset` | float | Z offset |
| `extruder` | string | Associated extruder name |
| `fan` | string | Associated fan name |

### G-code Commands

| Command | Action |
|---------|--------|
| `SELECT_TOOL TOOL=T{n}` | Mount specified tool |
| `UNSELECT_TOOL` | Unmount current tool (park it) |
| `T{n}` | Tool change macro |

### Path Topology

`PathTopology::PARALLEL` -- Each slot has its own independent path to a separate toolhead. No converging path visualization needed.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | No | -- |
| Tool Mapping | No | Fixed (tools ARE slots) |
| Bypass Mode | No | Not applicable |
| Spoolman | No | -- |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | No | -- |

### Discovery Sequence

Tool names must be provided via `set_discovered_tools()` before calling `start()`. The caller (typically `AmsState::init_backend_from_hardware()`) extracts tool names from `PrinterDiscovery::get_tool_names()`.

---

## AD5X IFS (FlashForge Adventurer 5X)

> **Status: TESTING** — This backend is functional but not yet fully supported. It is available for user testing and feedback. Please report issues via GitHub.

The AD5X has a 4-lane Intelligent Filament Switching (IFS) system controlled by a separate STM32 MCU. HelixScreen supports it through ZMOD firmware (ghzserg's Klipper mod for FlashForge printers).

> **Required firmware**: [ZMOD open-source firmware](https://github.com/ghzserg/zmod) **v1.7.0 or newer** (v1.7.0, Mar 2026, is the first release with explicit HelixScreen integration via `DISPLAY_OFF HELIX=1`). Hard minimum: v1.6.2 (Oct 2025), when the `less_waste_*` `save_variables` plumbing first appeared via the bambufy plugin — older versions are missing the slot-color/material surface we read.
>
> Note: this is ZMOD's own version, not FlashForge stock firmware. ZMOD supports stock AD5X bases from v1.0.2 (Jan 2025) onward; no specific FF stock version is required.

### Detection

IFS is detected via `filament_switch_sensor _ifs_port_sensor_{1-4}` or `filament_motion_sensor _ifs_motion_sensor_{1-4}` in `printer.objects.list`. The leading space in sensor names is intentional — it's a Klipper object naming convention.

Detection is gated by `!has_mmu_` — if Happy Hare or AFC is already detected, IFS sensors are ignored (priority: HH > AFC > IFS).

### State Sources

Stock zMod owns two Klipper objects — `zmod_ifs` and `zmod_color` — that hold the authoritative per-channel state, but their rich APIs are `printer.lookup_object()`-only (no `get_status()`), so Moonraker cannot see them. What Moonraker actually exposes depends on whether the lessWaste / bambufy plugins are installed.

**Shared (stock zMod and plugins both provide):**

| Source | Data | Notes |
|--------|------|-------|
| `filament_switch_sensor head_switch_sensor` | Toolhead filament presence | Authoritative NOZZLE/TOOLHEAD indicator |
| `filament_motion_sensor ifs_motion_sensor` | Filament moving **post-hub**, inside the IFS | Single boolean on stock zMod. Maps to `OUTPUT` segment — **not** the toolhead. Replaced by per-port sensors when plugins are installed. |
| `Adventurer5M.json` (Moonraker file API) | Per-channel color + material type | Polled + re-read on sensor edges / gcode responses. No push notifications. |

**Plugin-only (lessWaste / bambufy) — the Moonraker-visible export of `zmod_ifs` / `zmod_color`:**

| Source | Data | Plugin delta over stock zMod |
|--------|------|------------------------------|
| `filament_switch_sensor _ifs_port_sensor_{1-4}` | Per-port HUB presence (4 booleans) | Wraps `zmod_ifs.ifs_data.get_port(port)` — invisible to Moonraker otherwise |
| `save_variables.<prefix>_colors` / `_types` | Atomic per-port color + material | Subscribable; stock requires json polling |
| `save_variables.<prefix>_tools` | 16-element tool→port map | Not exposed on stock zMod |
| `save_variables.<prefix>_current_tool` | Active tool index (-1 or 0-15) | Stock: `zmod_color.get_current_channel()` (lookup-only) |
| `save_variables.<prefix>_external` | Bypass / external mode flag | Stock: `zmod_color.get_printer_data_detail().indepMatlInfo` (lookup-only) |
| `_IFS_VARS` gcode macro | Atomic writes of the above | Stock lacks this — can't persist UI-side changes |

Prefix is `less_waste` (lessWaste / zmod) or `bambufy` (bambufy); the schema is identical. Auto-detected from whichever keys are present.

> **Upstream wishlist:** add `get_status()` to `zmod_ifs` and `zmod_color` in stock zMod. That would close the plugin gap entirely and let HelixScreen drop the `Adventurer5M.json` polling path. Until then, users without a plugin see a degraded UI (no per-port HUB presence, no live tool map, no bypass flag, no atomic color updates).

> **Sensor-location correction:** the `ifs_motion_sensor` sits **inside the IFS immediately after the hub**, not at the toolhead. The current backend routes it through `parse_head_sensor()` as a simplification; a proper fix would map it to `PathSegment::OUTPUT` and require the toolhead switch for `filament_loaded` / load-complete detection.

#### The two data sources, and which one owns what

On native ZMOD (no lessWaste / bambufy plugin) the backend reconciles **two** independent reads. They answer different questions and must not be confused — conflating them is the root of the resurrection bug documented below.

| Source | Transport | Question it answers | Code |
|--------|-----------|---------------------|------|
| `Adventurer5M.json` `FFMInfo` | Moonraker `download_file("config", "Adventurer5M.json")`, 5s content-compare poll | What **color / material** is *assigned* to each channel (persisted metadata) | `parse_adventurer_json()`, `poll_adventurer_json()`, `note_json_content()` |
| `GET_ZCOLOR SILENT=1` (and, future, `IFS_STATUS`) | gcode console, on-demand | Which lanes **physically have filament** (RS-485 silk sensor) + the active lane | `query_zcolor_silent()`, `parse_zcolor_silent()`, `apply_zcolor_result()` |

**`Adventurer5M.json` `FFMInfo` has NO per-channel presence field.** It carries `ffmColor{1-4}` / `ffmType{1-4}` plus an active `channel`, and those colors **persist across unload/eject** — zmod never blanks `ffmColorN` when a lane is emptied. So a non-empty `ffmColorN` means "this channel was *assigned* this color", **not** "filament is loaded here". (Field-proven on raza616's hardware: he ejected *and* unloaded channel 1, yet `ffmColor1` stayed populated; a live `IFS_STATUS` reported `Ports:[F,T,T,T]` while the JSON still had `ffmColor1` set — the JSON simply does not track presence.)

**`GET_ZCOLOR SILENT=1`** is the silk-sensor truth. Text format (every line `// `-prefixed), parsed by `parse_zcolor_silent()`:

```
// Extruder: 3: PLA/2750E0 | IFS: True   <- summary: active lane, its mat/hex, IFS-mode flag
// 1: PLA/FFFFFF                          <- one row per LOADED slot (silk-detected)
// 3: PLA/2750E0
```

- Summary `Extruder: None (N)` = nothing at the hotend; `Extruder: N: MAT/HEX` = slot N is feeding the head. The `(N)` paren form carries the current channel.
- A **missing slot number = empty** — zmod filters slot rows by `hasFilament` from the RS-485 `silk_state` bitmask, so an absent row is the presence signal for "this lane is physically empty".
- Slot body is `MATERIAL`, `MATERIAL/HEX`, or `MATERIAL/NAME/HEX`. Material is everything before the first `/`; hex is everything after the **last** `/`. A response with slot rows but no `/HEX` is flagged `is_old_format` (pre-zmod-`ad2802ab`, Apr 2026) — presence only, colors still come from JSON.
- A response whose lines contain `action:prompt_` means **old zmod returned the interactive dialog instead of silent text** → `is_prompt_fallback` (see presence-ownership rule below).

**`IFS_STATUS`** (`zmod_ifs.py` `cmd_IFS_STATUS` → `ifs_data.get_values()`) is a cleaner, structured alternative that ships in zmod 1.7.1 but is **not yet consumed** by HelixScreen. It returns clean JSON:

```json
{"State": 4, "Ports": [false, true, true, true], "Silk": 14,
 "Chan": 4, "Insert": 0, "NeedInsert": false, "Stall": false, "stall_state": 0}
```

`Ports[i]` is `(silk_state >> i) & 1` — the same RS-485 bits `GET_ZCOLOR` filters on, but already decoded to booleans. `Chan` is the active port. **This is the future presence source**: it would let the backend drop both the `GET_ZCOLOR` text-scrape *and* the 5s JSON poll. Tracked as the firmware-integration headline; the upstream wishlist below (`get_status()` on `zmod_ifs`) would close the gap entirely.

#### Presence ownership rule (and the resurrection bug)

> **Presence is owned SOLELY by `GET_ZCOLOR` on modern zmod.** `parse_adventurer_json()` must NOT infer presence from `ffmColorN`. This is the fix for the channel-resurrection bug (commits `35dfcb765`, `2081e5757`).

**The bug.** Earlier code in `parse_adventurer_json()` treated a non-empty `ffmColorN` as `port_presence_[idx] = true` with no guard. Because zmod persists `ffmColorN` across unload/eject, an emptied lane was **resurrected as loaded on every content-changed poll**. The exact field report (raza616, v0.99.78): he externally unloaded channel 1 (Helix failed to clear it), then a `FIRMWARE_RESTART` fixed it (a fresh `GET_ZCOLOR` set `port_presence_[0]=false`), but then **editing channel 4's color in zmod changed the JSON content → triggered a reparse → the persisted `ffmColor1` resurrected channel 1 with its stale color**. One edit resurrected an unrelated emptied lane.

**The fix.** On modern zmod (where `GET_ZCOLOR SILENT=1` works), `parse_adventurer_json()` refreshes `colors_[]` / `materials_[]` only and leaves `port_presence_` untouched. `apply_zcolor_result()` (the silk-sensor read) is the sole presence authority, and it now also drives the `present→absent` override-clear (`clear_override_locked`) that used to ride on the JSON inference. Every JSON content change already schedules a `GET_ZCOLOR` immediately after the parse (`poll_adventurer_json()` calls `schedule_zcolor_query()`), so silk-truth presence re-establishes on the same event — the JSON setting presence was both **wrong and redundant**.

**The pre-SILENT regression (caught in review → commit `2081e5757`).** Making `GET_ZCOLOR` the sole authority breaks presence *entirely* on **old zmod**, where `GET_ZCOLOR SILENT=1` returns a prompt dialog instead of silent text. There, `apply_zcolor_result()` sees `is_prompt_fallback`, latches `zcolor_silent_supported_ = false`, and every subsequent `schedule_zcolor_query()` / `query_zcolor_silent()` no-ops forever. With JSON inference removed, every channel would be stuck EMPTY. The fix **gates the legacy `ffmColorN` inference on `!zcolor_silent_supported_`** (`parse_adventurer_json()`, the `if (!has_per_port_sensors_ && !zcolor_silent_supported_.load())` block):

- **Modern zmod** (`SILENT` works): `GET_ZCOLOR` owns presence; JSON never touches it → resurrection fixed.
- **Pre-SILENT zmod** (`zcolor_silent_supported_` latched false): no silk query exists, so JSON inference is the only fallback — `non-empty color == present`, `empty color while IDLE == eject + override-clear`. The resurrection bug can't bite here because no `GET_ZCOLOR` competes for ownership.

> **Known minor edge (accepted):** on modern zmod, an external color edit that arrives via the JSON poll *before* `GET_ZCOLOR` has confirmed presence won't sync to `lane_data` (the baseline moves without syncing). Rare, and far preferable to the constant resurrection. Confirmed acceptable in review.

**save_variables keys** (all prefixed `less_waste_`):

| Key | Type | Example |
|-----|------|---------|
| `less_waste_colors` | string[] | `['FF0000', '00FF00', '0000FF', 'FFFFFF']` |
| `less_waste_types` | string[] | `['PLA', 'PETG', 'ABS', 'TPU']` |
| `less_waste_tools` | int[16] | `[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]` |
| `less_waste_current_tool` | int | `0` (T0), `-1` (none) |
| `less_waste_external` | int | `0` (IFS mode), `1` (bypass/external) |

Tool mapping: array index = tool number (T0-T15), value = physical port (1-4, 5=unmapped).

### G-code Commands

| Command | Action |
|---------|--------|
| `INSERT_PRUTOK_IFS PRUTOK={port}` | Load filament from port (looks up temp from config) |
| `IFS_REMOVE_PRUTOK` | Toolhead unload — retract the currently-loaded filament |
| `REMOVE_PRUTOK_IFS PRUTOK={port}` | Toolhead unload (heat + retract the currently-loaded filament). **Not** a per-port jog — `PRUTOK=N` does not eject an idle lane; see note below |
| `IFS_F11 PRUTOK={port} LEN={mm} SPEED={s} CHECK=0` | Cold per-lane retract — reverse one idle lane's feed motor toward the spool; no heat, no presence guard. Used for idle-lane recovery (#996) |
| `A_CHANGE_FILAMENT CHANNEL={port}` | Full tool change |
| `SET_EXTRUDER_SLOT SLOT={port}` | Select slot without loading |
| `IFS_UNLOCK` | Reset IFS driver state machine |
| `_IFS_VARS key=value SHOW=0` | Persist color/type/tool/external changes |

**Variable persistence**: Use `_IFS_VARS` macro (not raw `SAVE_VARIABLE`) to persist slot data. `_IFS_VARS` updates both in-memory gcode variables AND `save_variables` with the correct prefix (`less_waste_*` for lessWaste/zmod, `bambufy_*` for bambufy). `SHOW=0` suppresses the interactive dialog. Example: `_IFS_VARS colors="['FF0000', '00FF00']" SHOW=0`.

**Plugin compatibility**: HelixScreen auto-detects the variable prefix from whichever `save_variables` are present on the printer. Both lessWaste and bambufy use the same schema, just different prefixes.

**Unload is toolhead-oriented, not per-lane**: Both `REMOVE_PRUTOK_IFS PRUTOK={port}` and `IFS_REMOVE_PRUTOK` run the toolhead unload sequence — they heat the hotend and retract whatever filament is currently loaded to the toolhead. The `PRUTOK={port}` argument does **not** select an idle lane to jog independently; observed on a real AD5X (native ZMOD), `REMOVE_PRUTOK_IFS PRUTOK=N` unloaded the currently-loaded filament (in a different slot) and ignored port N, and can error `No filament N in IFS` when IFS state disagrees with presence. A cold per-lane retract, by contrast, **is** available at the gcode layer via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (core ZMOD — a thin wrapper over raw serial `F11 C{port}…` with no heating and, with `CHECK=0`, no presence guard). This is why HelixScreen keeps the currently-loaded slot unloadable after runout (#995); and #996 implements HelixScreen calling `IFS_F11` directly for idle-lane recovery (e.g. a snapped chunk stuck in a lane's feed path — no hot nozzle involved). See `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` §12.

#### Per-lane eject (`eject_lane()`)

A real per-lane eject — pulling a whole spool's worth of filament back out of an idle lane so the user can remove it by hand — is **not** a single `IFS_F11`. A bare `IFS_F11` defaults to `LEN=90` (`zmod_ifs.py` `cmd_IFS_F11`, `gcmd.get_int('LEN', 90)`), which barely moves the filament, and an **unclamped** gear doesn't grip at all. `eject_lane()` (commit `dfcc83c0f`) mirrors zmod's own `_REMOVE_PRUTOK_IFS` macro with a three-command sequence:

```
IFS_F24 PRUTOK={port}                          # clamp — the gear now grips
IFS_F11 PRUTOK={port} LEN={tube} SPEED={speed} # cold retract the FULL tube length (no heat, no home)
IFS_F39 PRUTOK={port}                          # unclamp — filament is free to pull out by hand
```

- **`LEN` / `SPEED` are per-material**, resolved from zmod's `/mod_data/filament.json` keyed by the lane's material type. Fetched once at startup by `fetch_filament_json()` (mirrors `read_adventurer_json` threading; 404 on non-zmod is silent), parsed by `parse_filament_json()` into `filament_eject_params_`. Structure:

  ```json
  { "default": { "filament_tube_length": 1000, "filament_ifs_speed": 1200, ... },
    "PLA":     { "filament_tube_length": 1000, "filament_ifs_speed": 1200, ... },
    "PETG":    { "filament_tube_length": 650,  "filament_ifs_speed": 1200, ... } }
  ```

  Resolution order: per-material entry → file's `default` entry → hardcoded **1000 / 1200** (`filament_eject_default_`). `filament_tube_length` is the PTFE-tube length from the IFS module to the extruder (zmod default 1000 mm) — users with non-stock tubes get the right distance automatically. (Field-confirmed on raza616: `LEN=1000` ran the full retract and ended free after `F39`; his PETG tube is 650.)

- **Eject refuses the toolhead-loaded active lane.** If `system_info_.current_slot == slot_index && head_filament_`, `eject_lane()` returns `WRONG_STATE` ("Lane is loaded in toolhead / Unload from toolhead first") — a cold backward retract would fight the loaded filament. Unload it from the toolhead first.

#### External-change triggers (the gcode-response listener)

`register_zcolor_listener()` subscribes to `notify_gcode_response`; `on_gcode_response_line()` schedules a `GET_ZCOLOR` re-read when it sees an externally-driven change (commit `cafcff3ad` added the bare-`Extruder:` trigger). The watched signals:

| Token in stream | Why it fires | Notes |
|-----------------|--------------|-------|
| `RUN_ZCOLOR` / `CHANGE_ZCOLOR` | Deliberate external color/material edit (AD5X LCD, Mainsail, zmod COLOR macro). HelixScreen persists colors by writing `Adventurer5M.json` directly and **never** emits these — so they can only be external. | `CHANGE_ZCOLOR SLOT=N` carrying a real locked override also clears that stale override so the new firmware color wins (#981). |
| bare `Extruder: <N>` | zmod's `_SET_EXTRUDER_SLOT` (`zmod_color.py` `cmd_SET_EXTRUDER_SLOT`) emits `Extruder: {zslot}` via `respond_raw` at the channel-commit step near the **end** of an operation. This is the marker that catches **external unloads/loads done via zmod's own color macro**, where the stream carries no `RUN_ZCOLOR`/`CHANGE_ZCOLOR`. | Matched by a **strict** regex (`^\s*(?://\s*)?Extruder:\s*\d+\s*$`). |

**Why `IN_ZCOLOR` is NOT watched.** An external unload via zmod's color macro emits `IN_ZCOLOR SLOT=N NAPR=0/1` (load/unload) — but the literal `IN_ZCOLOR` token only appears in the **dialog button *definition* echo** (`action:prompt_button Unload|IN_ZCOLOR SLOT=N NAPR=1…`) at *prompt-render* time, **not** when the unload actually runs. Watching it would false-fire on dialog-open and still miss the real unload. The bare `Extruder: <N>` channel-commit marker is the reliable terminal signal instead.

**Why the bare-`Extruder:` regex is strict.** It must NOT match the `GET_ZCOLOR SILENT` summary (`Extruder: ... | IFS: True`), the interactive prompt (`action:prompt_text Extruder: ... | IFS:`), or per-slot rows (`3: PLA/HEX`) — all of which carry a ` | IFS:` suffix or a different shape. Combined with the `zcolor_query_active_` early-return guard (which buffers our own in-flight `GET_ZCOLOR` response echoes), this keeps the v0.99.51 **self-feedback spam loop** closed: `GET_ZCOLOR` never emits a bare `Extruder: N`, so a re-read can't re-trigger itself. (HelixScreen's own `SET_EXTRUDER_SLOT` *does* emit a bare `Extruder: N`, but a re-read after a Helix-initiated tool change is harmless — `schedule_zcolor_query()` is debounced + idempotent.) The bg-side pre-filter in `register_zcolor_listener()` admits the cheap `Extruder:` substring; the strict regex runs on the main thread in `on_gcode_response_line()`. **If you add a new trigger, update both the bg-side admit filter and the main-thread branch** — otherwise the new token is silently dropped on busy print streams.

#### zmod IFS command reference

The raw IFS commands (`zmod_ifs.py` registrations + `docs/en/AD5X.md`). `F##` numbers are thin wrappers over raw serial `F## C{port}…`. Not every raw `F##` the IFS firmware accepts is exposed as a zmod gcode macro — e.g. `F19` is used at the raw-serial layer but has no `IFS_F19` command (community knowledge, ninjamida's multi-IFS project); only the subset ZMOD actually drives is wrapped:

| Command | Action |
|---------|--------|
| `IFS_F10` | Insert filament (`F10 C{port} L{len} S{speed}`) |
| `IFS_F11 [LEN=mm] [SPEED=s] [CHECK=0/1]` | Remove/retract filament. **`LEN` defaults to 90** (barely moves) — pass the tube length for a full eject |
| `IFS_F13` | Query IFS state |
| `IFS_F24 PRUTOK=N` | Clamp the lane (gear grips) |
| `IFS_F39 PRUTOK=N` | Unclamp one lane (filament free to pull) |
| `IFS_F18` | Unclamp **all** lanes at once — no `PRUTOK` needed. ⚠️ zmod's `docs/en/AD5X.md` mistranslates this as "Filament purge everywhere"; the actual handler (`cmd_IFS_F18`) responds *"Unlocking all filaments"*. Handy for recovery when you don't know which lane is clamped (community tip, ninjamida) |
| `IFS_F112` | Stop filament feed |
| `IFS_STATUS` | Structured JSON state (`State`/`Ports`/`Silk`/`Chan`/…) — clean future presence source |
| `GET_ZCOLOR SILENT=1` | Per-slot loaded state + active lane as `// `-prefixed text (silk-sensor truth) |
| `REMOVE_PRUTOK_IFS PRUTOK=N` | Toolhead unload (heat + retract); **not** a per-lane jog |
| `IN_ZCOLOR SLOT=N NAPR=0/1` | zmod color-macro load (`NAPR=0`) / unload (`NAPR=1`) — emitted on the AD5X LCD / Mainsail path |

### Path Topology

```
  Port 1 ──┐
  Port 2 ──┤
            ├── Combiner ── Toolhead
  Port 3 ──┤
  Port 4 ──┘
```

`PathTopology::LINEAR` — 4 independent lanes merge at a single combiner before the toolhead.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | No | -- |
| Tool Mapping | Yes | Yes (16 tools → 4 ports) |
| Bypass Mode | Yes | Via `less_waste_external` |
| Spoolman | Optional | Works if configured |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | No | -- |

### Open Issues & Debugging Notes

> **No AD5X test device.** HelixScreen ships IFS support **blind** — there is no AD5X in the test fleet. Every IFS fix is field-validated through users, primarily **raza616** (the most active AD5X/IFS reporter). Treat live Discord console pastes and freshly-captured debug bundles as the ground truth, and prefer regression tests + the mock backend (`HELIX_MOCK_AMS=ifs`) for anything that can't be exercised on hardware.

**Stuck-purge on load — KNOWN OPEN, UNRESOLVED.** Loading a lane via the multi-filament screen can leave HelixScreen stuck displaying "purging" indefinitely. **No confirmed cause; do not ship a speculative fix.** Dead ends already ruled out:

- *Head-sensor-clobber theory* — **disproved.** raza's live `QUERY_FILAMENT_SENSOR` showed both `head_switch_sensor` and `ifs_motion_sensor` detecting filament when loaded. (Bundle snapshots show `head_switch=false` in all three captures, but that reconciles to "nothing at the head *at capture time*" — raza had already unloaded — not a sensor inversion.)
- *`BlockingIOError [Errno 11]` at `gcode.py:459 _respond_raw`* — **red herring.** It's present in the *not*-stuck bundle too, in a bed-mesh-dump context. It's console-flood backpressure (drops echoed text, not state).

To actually crack it, capture — **while stuck**:

1. A debug bundle whose `log_tail` is **verified fresh** (its last timestamp == the bundle timestamp; see caveat below).
2. A simultaneous live `QUERY_FILAMENT_SENSOR` for both `head_switch_sensor` and `ifs_motion_sensor`.
3. A live `IFS_STATUS`.
4. zmod's own `/var/log/messages` — **the "did the load actually finish inside zmod" answer lives here, NOT in the bundle.**

> **Debug-bundle caveat (cost a whole investigation):** the bundle's `log_tail` can be a **stale ring buffer that predates the incident**. In the stuck-purge bundles the `log_tail` ended *before* the reported event, so the `RUN_ZCOLOR` / `IN_ZCOLOR` / `ActionPrompt` lines "read from the log" were an **old session**, and `klipper_log` was just a 74-second idle window (config dump + bed-mesh table). **Always confirm the `log_tail`'s last timestamp matches the bundle timestamp before trusting any "from the log" claim.** When the bundle is stale, the only valid runtime evidence is live console pastes.

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_ad5x_ifs.h` | Backend class declaration |
| `src/printer/ams_backend_ad5x_ifs.cpp` | Full implementation |
| `tests/unit/test_ams_backend_ad5x_ifs.cpp` | Unit tests (16 cases, 100+ assertions) |
| `docs/devel/printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | Protocol research |

### Automatic Setup

AD5X users running ZMOD firmware get automatic detection — no configuration needed. When HelixScreen connects to a Moonraker instance with IFS sensors, it:

1. Detects `filament_switch_sensor _ifs_port_sensor_*` in object list
2. Sets `AmsType::AD5X_IFS`
3. Subscribes to `save_variables` for filament state
4. Creates `AmsBackendAd5xIfs` backend
5. Queries initial state via `printer.objects.query`

Existing beta testers upgrading to a version with IFS support will see the filament panel populate automatically on next connection.

---

## CFS (Creality Filament System)

Two distinct firmware dialects share the `box` Klipper object. HelixScreen routes between them at backend construction:

| Printer family | Stock firmware path | Macro dialect | Detection signal |
|----------------|--------------------|---------------|-----------------|
| K2, K2 Pro, K2 Plus, K2 Max (built-in CFS) | Creality K2 firmware | `CR_BOX_*` primitives + `BOX_SAVE_FAN`/`BOX_MODE_WAIT` envelope | `PrinterDetector::is_creality_k1() == false` |
| K1, K1C, K1 Max (official CFS upgrade ≥ v2.3.5.33) | Creality K1 CFS upgrade firmware | Plain `BOX_*` primitives, no fan-save/mode-wait | `PrinterDetector::is_creality_k1() == true` |

### Firmware requirements

- **K2 series:** Stock firmware. Detection is automatic when the CFS unit is paired (RS-485, exposes `box` Klipper object).
- **K1 series:** Requires the **official Creality K1/K1C/K1 Max CFS upgrade firmware** (the reporter for #968 had `v2.3.5.33`). Stock K1/K1C/K1Max firmware without the CFS upgrade does not expose the `box` object and the backend stays disabled. Community open-source K1 firmwares (Guilouz, etc.) do not currently bundle the CFS macros — install Creality's signed CFS-aware image to use the upgrade.

### Macro dialect comparison

| Operation | K2 emission | K1 emission |
|-----------|-------------|-------------|
| Envelope open | `SAVE_GCODE_STATE` → `BOX_SAVE_FAN` → `BOX_GO_TO_EXTRUDE_POS` → `BOX_MODE_WAIT` | `SAVE_GCODE_STATE` → `BOX_GO_TO_EXTRUDE_POS` |
| Load slot N | `CR_BOX_PRE_OPT` → `CR_BOX_EXTRUDE TNN=…` → `CR_BOX_WASTE` → `CR_BOX_FLUSH TNN=…` → `CR_BOX_END_OPT` | `BOX_EXTRUDE_MATERIAL TNN=…` → `BOX_MATERIAL_FLUSH TNN=…` |
| Unload current | `CR_BOX_PRE_OPT` → `CR_BOX_CUT` → `BOX_MODE_WAIT` → `CR_BOX_RETRUDE` → `CR_BOX_END_OPT` | `BOX_CUT_MATERIAL` → `BOX_RETRUDE_MATERIAL` |
| Envelope close (with wipe) | `BOX_NOZZLE_CLEAN` → `BOX_RESTORE_FAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` | `BOX_NOZZLE_CLEAN` → `BOX_MOVE_TO_SAFE_POS` → `RESTORE_GCODE_STATE` |
| Tool remap | `BOX_MODIFY_TN T<src>=T<dst>` | (same — assumed; needs field confirmation) |
| Color sync | `BOX_MODIFY_TN_DATA ADDR=… NUM=… PART=color_value DATA=0RRGGBB` | (same — assumed; needs field confirmation) |

The K1 envelope is intentionally shorter — `BOX_SAVE_FAN` / `BOX_RESTORE_FAN` / `BOX_MODE_WAIT` are not exposed by the K1 CFS firmware (verified absent in the public K1-Max box.cfg dump at [DieDutchman/K1-Max-KAMP-CFS-Fix](https://github.com/DieDutchman/K1-Max-KAMP-CFS-Fix/blob/main/Config_Files/box.cfg) and from the #968 reporter's gcode/help output). Emitting them on K1 would surface as `key61 Unknown command`.

### Implementation

| File | Role |
|------|------|
| `include/ams_backend_cfs.h` | `CfsMacroVariant` enum, `AmsBackendCfs` class, static `load_gcode/unload_gcode/swap_gcode(idx, variant)` helpers |
| `src/printer/ams_backend_cfs.cpp` | `wrap_with_park_k1` / `wrap_with_park_k2` envelopes, K1-vs-K2 body emission |
| `include/printer_discovery.h` | `box` object handler — enables CFS for both K1 and K2 (#968 gate flipped) |

`AmsBackendCfs::macro_variant_` is latched in the constructor by querying `PrinterDetector::is_creality_k1()`. All member operations (`load_filament`, `unload_filament`, `change_tool`) thread `macro_variant_` into the gcode helpers. Static call sites without an explicit variant default to `K2` to preserve existing test behavior.

### Known limitations on K1

- `BOX_MODIFY_TN` (tool remap) and `BOX_MODIFY_TN_DATA` (color sync) are emitted with the same syntax on K1 — neither has been field-validated.
- `BOX_LOAD_MATERIAL_WITH_MATERIAL` and `BOX_QUIT_MATERIAL` (K1 high-level orchestrators) are not used; HelixScreen drives the primitives directly to keep behavior parallel between the two backends.
- Bed-area shrink for the rear-mounted K1 CFS upgrade (~5 mm Y) is not yet applied via the printer database.
- Hardware validation for K1/K1C is pending — track via [#968](https://github.com/prestonbrown/helixscreen/issues/968).

---

## QIDI Box (QIDI PLUS4 / Q2 / MAX4)

> **Status: STUB** — The `AmsType::QIDI_BOX` enum value, factory wiring, and a no-op `AmsBackendQidi` scaffold exist so the type round-trips through the rest of the system. No real protocol is implemented. Every backend operation logs `spdlog::warn("... not yet implemented")` and returns `AmsErrorHelper::not_supported(...)`. Do **not** ship this as a user-facing feature until live hardware validation has happened.

The QIDI Box is QIDI's RFID-aware multi-material system: 4 slots per unit, chainable up to 4 units = 16 colors, with active drying up to 65°C and runout/tangle sensors. It is a **hub-style AMS** (like FlashForge IFS or Bambu AMS), not a lane-selector MMU — the closest in-tree analog is `AmsBackendAd5xIfs`, not Happy Hare or AFC.

### Compatible Hardware

| Printer | Supported | Notes |
|---------|-----------|-------|
| QIDI PLUS4 | Yes (per QIDI) | PLUS4 kit is not interchangeable with Q2/MAX4 — different hub board + data cable |
| QIDI Q2    | Yes (per QIDI) | Same kit as MAX4 |
| QIDI MAX4  | Yes (per QIDI) | Same kit as Q2 |
| Q1 Pro     | **No** | Unsupported by QIDI — different mainboard generation |
| X-Max 3    | **No** | Unsupported by QIDI — older MKSPI board |

The `assets/config/printer_database.json` entries for `qidi_plus_4` and `qidi_q2` carry an `"ams_type": "qidi_box"` capability tag. A `qidi_max_4` entry does not yet exist in the database — add one alongside the real protocol work.

### Detection

**Not yet wired.** The `ams_type` capability in the printer database is informational today — actual filament-system detection runs through heuristics in `include/printer_discovery.h` against `printer.objects.list`. Detection for QIDI Box will likely key off Klipper objects exposed by the Box's udev-identified USB-serial device (`QIDI_BOX_V1`) or the `_BOX_*` gcode macros. The exact object names need to be enumerated on a real PLUS4 / Q2 / MAX4.

### Firmware Openness

QIDI printers (Q1 Pro and newer) run forks of Klipper and Moonraker from [QIDITECH/klipper](https://github.com/QIDITECH/klipper) and [QIDITECH/moonraker](https://github.com/QIDITECH/moonraker). SSH is open by default (`mks` / `makerbase`), and KIAUH is pre-installed. QIDI discourages upstream Klipper updates because their board requires their fork.

**The Box firmware itself ships as obfuscated `.so` Python extension modules.** A community open-source reimplementation at [qidi-community/Plus4-Wiki customisable_qidibox_firmware](https://github.com/qidi-community/Plus4-Wiki/tree/main/content/customisable_qidibox_firmware) replaces six modules (`box_detect.py`, `box_rfid.py`, `box_stepper.py`, `box_extras.py`, `aht20_f.py`, `buttons_irq.py`) with editable Python. Maintainers label it "strongly WIP." This repo is the primary protocol reference for a HelixScreen integrator.

### Control Surface (expected)

All control runs through Klipper gcode macros — **no dedicated Moonraker endpoints**, no REST extension. State lives in printer objects and `save_variables`, same shape as AD5X IFS. Known macro names from the QIDI stock config:

| Command | Action |
|---------|--------|
| `BOX_CHANGE_FILAMENT` | Tool change |
| `_BOX_START` | Internal helper |
| `_BOX_*` | Additional internal macros |

Exact parameter shapes and the full macro list need to be confirmed against a real printer.

### Path Topology

```
  Slot 1 ──┐
  Slot 2 ──┤
            ├── Hub ── Toolhead
  Slot 3 ──┤
  Slot 4 ──┘
```

`PathTopology::HUB` — slots converge at a hub inside the Box before the toolhead. Chained boxes add units with their own hubs; the multi-unit addressing scheme is not publicly documented.

### RFID

Spools identify via MIFARE Classic RFID tags. Data lives in sector 1 block 0. Third-party read/write tools exist:

- [TinkerBarn/BoxRFID](https://github.com/TinkerBarn/BoxRFID) — Electron desktop app
- [n0cloud/qidi-box-rfid-manager](https://github.com/n0cloud/qidi-box-rfid-manager) — mobile
- [LexyGuru/Qidi_RFID_App](https://github.com/LexyGuru/Qidi_RFID_App)

### Do NOT Confuse With

- **Happy Hare "QuattroBox"** — listed in Happy Hare's supported hardware, but it is an unrelated DIY MMU by [Batalhoti](https://github.com/Batalhoti/QuattroBox). Happy Hare does **not** support the QIDI Box.
- **The `"box"` string alias in `ams_type_from_string()`** — already claimed by `CFS` (Creality K2 "box" terminology). QIDI Box requires the explicit `"qidi_box"` / `"QIDI Box"` / `"qidibox"` spelling.

### Capabilities (planned)

| Feature | Expected | Notes |
|---------|----------|-------|
| Endless Spool | Yes (auto-backup-spool) | Advertised by QIDI |
| Tool Mapping | Likely via `save_variables` | Matches AD5X IFS shape |
| Bypass Mode | Unknown | Need hardware inspection |
| Spoolman | Optional | Works through standard Moonraker `[spoolman]` |
| Auto-Heat on Load | Unknown | |
| Dryer | Yes (up to 65°C) | `aht20_f.py` owns humidity sensing |
| Device Actions | Unknown | |

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_qidi.h` | Backend class declaration (stub) |
| `src/printer/ams_backend_qidi.cpp` | Stub implementation — logs warn, returns not-supported |
| `include/ams_types.h` | `AmsType::QIDI_BOX` enum + string converters |

No dedicated unit tests yet — adding them is blocked on having real protocol behavior to test against.

### Follow-up Work (in order)

1. Get access to a PLUS4, Q2, or MAX4 with a Box attached.
2. SSH in (`mks` / `makerbase`), enumerate `printer.objects.list` and `save_variables` keys. Capture the stock gcode macro bodies.
3. Add detection to `PrinterDiscovery::parse_objects()` — key off whatever Klipper objects the Box exposes.
4. Implement `AmsBackendQidi` on top of `AmsSubscriptionBackend`, modeled on `AmsBackendAd5xIfs` (printer-object polling + macro invocation).
5. Add the `qidi_max_4` entry to `assets/config/printer_database.json`.
6. Add `assets/images/ams/qidi_box_64.png` (TODO comment exists in the stub).
7. Write unit tests against captured real-device fixtures.

---

## Dryer / Box-Heater Control

Some AMS backends include an integrated filament dryer — a heated chamber that removes moisture from hygroscopic filaments (Nylon, PA-CF, TPU, PETG, etc.) before or during a print. HelixScreen exposes a common dryer control UI across all backends that support it.

### Data Flow

```
AmsBackend::get_dryer_info()       populates DryerInfo (ams_types.h)
        │
        ▼
AmsState::sync_dryer_from_backend()  bridges to LVGL subjects
        │
        ▼
AmsEnvironmentOverlay              control UI (ui_ams_environment_overlay.cpp
  + ui_xml/ams_environment_overlay.xml)  target temp, duration, start/stop
```

`DryerInfo` (declared in `include/ams_types.h`) carries:

| Field | Type | Description |
|-------|------|-------------|
| `supported` | bool | Whether this backend has a dryer at all |
| `active` | bool | Dryer is currently running |
| `current_temp` | float | Current chamber temperature (°C) |
| `target_temp` | float | Target setpoint (°C) |
| `remaining_minutes` | int | Countdown to end of session (-1 = no timer) |
| `max_temp` | float | Hardware maximum for the target slider |

> **Humidity is not on `DryerInfo`.** It lives on `EnvironmentData` (per-unit, `AmsUnit::environment`) alongside the box temperature, because humidity is a per-enclosure reading from an environment sensor, not a property of the global dryer session. The dryer overlay and the AMS panel environment indicator both read `AmsUnit::environment`.

### Backend Virtual Interface

Declared in `include/ams_backend.h`. Default implementations return `supported=false` / `not_supported` so existing backends that don't have a dryer need no changes.

| Virtual | Default | Description |
|---------|---------|-------------|
| `get_dryer_info()` | `DryerInfo{.supported=false}` | Read current dryer state |
| `start_drying(temp, minutes)` | NOT_SUPPORTED | Begin a drying session |
| `stop_drying()` | NOT_SUPPORTED | End the active session |
| `update_drying(temp, minutes)` | NOT_SUPPORTED | Change temp/time mid-session |
| `get_drying_presets()` | empty vector | Return material-preset list |

### Backend Support Matrix

| Backend | Drying | Command |
|---------|--------|---------|
| ACE (Anycubic ACE Pro) | ✅ | `ACE_START_DRYING TEMP=<t> DURATION=<m>` / `ACE_STOP_DRYING` |
| Happy Hare | ✅ | `MMU_HEATER DRY=1 TEMP=<t> TIMER=<mins>` / `MMU_HEATER STOP=1` (TIMER is minutes; see [Happy Hare Specifics](#happy-hare-specifics)) |
| QIDI Box | ✅ | `ENABLE_BOX_DRY BOX=<n> TEMP=<t> END_TIME=<h>` / `DISABLE_BOX_DRY BOX=<n>`, with `SET_HEATER_TEMPERATURE` fallback when `box_extras` is absent. Write-path always enabled (commands verified vs QIDI firmware, #1030). |
| CFS (Creality K2) | ❌ | Not supported — CFS has no drying hardware |
| AFC (Box Turtle / OpenAMS) | ❌ | Not supported |
| AD5X IFS | ❌ | Not supported |
| Snapmaker U1 (SnapSwap) | ❌ | Not supported |
| Tool Changer | ❌ | Not applicable |

### QIDI Box Specifics

The QIDI Box dryer uses the printer's standard `heater_generic heater_box<N>` Klipper object — the same safety system (temperature limits, watchdog) that applies to any Klipper heater. The active session timer is tracked via `box_extras.box_drying_state.box<N>` (fields `dry_state` and `end_time`). Remaining time is computed as `(end_time - now) / 60` since there is no native remaining-minutes field.

See [QIDI_BOX_HEATER.md](QIDI_BOX_HEATER.md) for full reverse-engineering details: Klipper object schema, firmware command variants, config key spellings, and per-material drying tables.

### Happy Hare Specifics

Happy Hare's filament dryer is driven by the `MMU_HEATER` command and configured under `[mmu_machine]`. HelixScreen reads the dryer's object names from `configfile.settings` once at connect (`query_heater_config_from_config`), then tracks live state over the normal subscription push — **there is no polling**.

**What HelixScreen supports today:**

- **Commands.** `MMU_HEATER DRY=1 TEMP=<°C> TIMER=<minutes>` to start, `MMU_HEATER STOP=1` to stop. `TIMER` is **minutes** (float, `minval=0`), so sub-hour cycles are valid — the duration field in the overlay is a minutes field for this reason.
- **Box temperature.** Read from the `filament_heater` `heater_generic` object's live `temperature`.
- **Box humidity.** Happy Hare's `mmu` object deliberately does **not** republish temp/humidity (`mmu_environment_manager.get_status()` omits them by design); the client must read the environment sensor object directly. Humidity therefore comes from the **backing humidity chip** — `bme280` / `htu21d` / `sht3x` / `aht10` `<name>`, where `<name>` is the bare second token of the `environment_sensor` value (e.g. `temperature_sensor box` → `htu21d box`). Discovery subscribes these chips and requests the `humidity` field on all temperature sensors (only objects present in `objects.list` are subscribed, so this is safe for printers without them).
- **Per-unit / multi-MMU resolution.** Happy Hare has two mutually-exclusive enclosure forms: a **shared** enclosure (scalar `filament_heater` / `environment_sensor`) or **per-gate** hardware (plural `filament_heaters` / `environment_sensors`, one entry per gate, distributed across units for multi-MMU). `get_system_info()` resolves **each unit's** heater + sensor — the scalar form applies to every unit; the per-gate lists map each unit to the object at its first gate (`first_slot_global_index`). So a multi-MMU rig with distinct box sensors shows the correct temp/humidity per unit in the panel indicator and overlay.

**What is NOT yet supported (boundary):**

- **Per-gate / per-slot / per-lane *drying control*.** The control surface (`AmsEnvironmentOverlay`) and the `DryerInfo` model are **single-dryer-global**: start/stop drives the default heater (no `GATES=` selector), and the per-gate `drying_state` **array** is collapsed to a single "any gate active" boolean in `parse_mmu_state`. Independently drying specific gates (`MMU_HEATER … GATES=g1,g2`), per-gate countdowns, and the `HUMIDITY=` termination target are tracked post-1.0 in **#1026**.

> Note: the per-unit environment *readout* (above) is unverified on per-gate/EMU hardware — it is unit-tested (scalar + 2-unit per-gate) but we own no EMU rig. The QIDI Box (the common Happy-Hare-on-a-box case) is a single shared sensor.

### Adding Dryer Support to a New Backend

Override the five dryer virtuals in your `AmsBackend` subclass. At minimum implement `get_dryer_info()` — the UI polls this to drive the display. Implement `start_drying()` and `stop_drying()` to make the controls functional. `update_drying()` and `get_drying_presets()` are optional enhancements.

Dryer status flows into `AmsState::sync_dryer_from_backend()` the same way slot state does — no additional wiring is required in `AmsState`.

---

## Context Menu Actions

The `AmsContextMenu` (`ui_ams_context_menu.h`) provides per-slot operations:

| Action | Description | Availability |
|--------|-------------|------------|
| **Load** | Load filament from this slot | When slot has filament and not at toolhead |
| **Unload** | Unload filament from extruder | When filament is loaded to extruder |
| **Eject** | Eject filament from hub back to spool (AFC only) | When hub-loaded but not at toolhead, and backend supports lane eject |
| **Spool Info** | View/edit slot properties (color, material, brand) | When slot has filament |
| **Spoolman** | Assign a Spoolman spool to this slot | Always |

The context menu also includes inline dropdowns for:

- **Tool Mapping**: Assign which tool number maps to this slot (if backend supports it)
- **Endless Spool Backup**: Set backup slot for runout (if backend supports it)

These dropdowns are populated from `backend->get_tool_mapping()` and `backend->get_endless_spool_config()`.

---

## Device Operations Overlay

The `AmsDeviceOperationsOverlay` (`ui_ams_device_operations_overlay.h`) consolidates device-specific controls:

### Fixed Actions (all backends)

| Action | G-code (varies by backend) | Description |
|--------|---------------------------|-------------|
| Home | `MMU_HOME` / `AFC_HOME` | Reset to home position |
| Recover | `MMU_RECOVER` / `AFC_RESET` | Attempt error recovery |
| Abort | `cancel()` | Cancel current operation |
| Bypass Toggle | `enable_bypass()` / `disable_bypass()` | Toggle bypass mode (if supported) |

### Dynamic Actions (backend-specific)

Each backend can expose dynamic device actions via `get_device_sections()` and `get_device_actions()`. The UI renders them as buttons, toggles, sliders, or dropdowns based on `ActionType`.

AFC exposes four sections: **Calibration**, **Speed Settings**, **Maintenance**, and **LED & Modes**. See the [AFC-Specific Features](#afc-specific-features) section for details.

---

## Mock Mode for Testing

The `AmsBackendMock` simulates any of the supported backend types for UI development and testing.

### Activation

Mock mode is activated when `RuntimeConfig::should_mock_ams()` returns true (typically via the `--test` CLI flag). The factory method `AmsBackend::create()` automatically returns a mock backend in this case.

### Environment Variables

| Variable | Values | Default | Description |
|----------|--------|---------|-------------|
| `HELIX_AMS_GATES` | 1-16 | 4 | Number of simulated slots |
| `HELIX_MOCK_AMS` | `afc`, `box_turtle`, `boxturtle`, `toolchanger`, `tool_changer`, `tc`, `mixed`, `multi`, `ifs`, `ad5x`, `ad5x_ifs` | Happy Hare | AMS type to simulate |
| `HELIX_MOCK_AMS_STATE` | `idle`, `loading`, `error`, `bypass` | `idle` | Visual scenario to simulate |
| `HELIX_MOCK_DRYER` | `1`, `true` | Disabled | Simulate integrated dryer |
| `HELIX_MOCK_DRYER_SPEED` | Integer | 60 | Dryer speed multiplier (60 = 1 real sec = 1 sim min) |

### Mock AFC Mode

```bash
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test
```

When AFC mock mode is enabled:

- Reports `AmsType::AFC` with type name "AFC (Mock)"
- Uses `PathTopology::HUB` (4 lanes merge through hub)
- Configures 4 lanes with realistic filament data (PLA, PETG, ABS, ASA)
- Sets AFC-specific device sections: Calibration, Maintenance, Speed Settings, LEDs & Modes
- Includes mock device actions: calibration wizard, bowden length slider, speed multipliers, lane tests, blade change, park, brush, motor reset, LED toggle, quiet mode toggle
- Uses `TipMethod::CUT`
- Editable endless spool with pre-configured backup mapping
- Supports auto-heat on load

### Mock Mixed Topology Mode

```bash
HELIX_MOCK_AMS=mixed ./build/bin/helix-screen --test
```

Simulates a real-world 6-toolhead toolchanger with mixed AFC hardware (based on production data):

- **Unit 0**: Box Turtle "Turtle_1" — 4 lanes, PARALLEL, lanes 0-3 → T0-T3, TurtleNeck buffers, no hub sensor
- **Unit 1**: OpenAMS "AMS_1" — 4 lanes, HUB, lanes 4-7 all → T4, per-lane hubs (Hub_1-Hub_4), no buffers
- **Unit 2**: OpenAMS "AMS_2" — 4 lanes, HUB, lanes 8-11 all → T5, per-lane hubs (Hub_5-Hub_8), no buffers
- Total: 12 slots, 6 physical toolheads
- Per-unit topology via `get_unit_topology()`
- 23 regression tests in `tests/unit/test_ams_mock_mixed_topology.cpp` validate this setup

### Mock Tool Changer Mode

```bash
HELIX_MOCK_AMS=toolchanger ./build/bin/helix-screen --test
```

- Reports `AmsType::TOOL_CHANGER`
- Uses `PathTopology::PARALLEL`
- Disables bypass mode
- Labels slots as "T0", "T1", etc.

### Mock AD5X IFS Mode

```bash
HELIX_MOCK_AMS=ifs ./build/bin/helix-screen --test
```

- Reports `AmsType::AD5X_IFS` with type name "AD5X IFS"
- Uses `PathTopology::LINEAR`
- 4 slots with bypass support
- Tool mapping enabled, endless spool disabled

### Mock Realistic Mode

```bash
HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test
```

Enables multi-phase operation simulation with realistic timing:

- **Load**: HEATING -> LOADING (segment animation) -> IDLE
- **Unload**: HEATING -> CUTTING -> UNLOADING (animation) -> IDLE
- Timing respects `--sim-speed` flag with +/-20-30% variance

### Mock-Specific Test Methods

The mock backend exposes additional methods for unit testing:

| Method | Description |
|--------|-------------|
| `simulate_error(AmsResult)` | Trigger a specific error condition |
| `simulate_pause()` | Set PAUSED state (user intervention required) |
| `resume()` | Resume from PAUSED state |
| `set_operation_delay(ms)` | Set simulated operation delay |
| `force_slot_status(slot, status)` | Force a specific slot status |
| `set_has_hardware_bypass_sensor(bool)` | Toggle hardware vs virtual bypass sensor |
| `set_endless_spool_supported(bool)` | Toggle endless spool support |
| `set_endless_spool_editable(bool)` | Toggle AFC-style (editable) vs Happy Hare-style (read-only) |
| `set_device_sections(sections)` | Set custom device sections for testing |
| `set_device_actions(actions)` | Set custom device actions for testing |

---

## Developer Guide: Adding a New Backend

### 1. Define the AmsType

Add a new value to `AmsType` in `ams_types.h`:

```cpp
enum class AmsType {
    // ... existing values ...
    MY_SYSTEM = 5  // New system type
};
```

Update `ams_type_to_string()`, `ams_type_from_string()`, and the `is_filament_system()` / `is_tool_changer()` helpers as appropriate.

### 2. Add Detection in PrinterDiscovery

In `printer_discovery.h`, add detection logic in `parse_objects()`:

```cpp
else if (name == "my_system") {
    has_mmu_ = true;
    mmu_type_ = AmsType::MY_SYSTEM;
}
```

Add any component discovery (lane names, tool names, etc.) as needed.

### 3. Implement the Backend Class

Create `include/ams_backend_mysystem.h` and `src/printer/ams_backend_mysystem.cpp`. Implement all pure virtual methods from `AmsBackend`:

**Required overrides:**

- `start()`, `stop()`, `is_running()` -- Lifecycle
- `set_event_callback()` -- Event registration
- `get_system_info()`, `get_type()`, `get_slot_info()`, `get_current_action()`, `get_current_tool()`, `get_current_slot()`, `is_filament_loaded()` -- State queries
- `get_topology()`, `get_filament_segment()`, `get_slot_filament_segment()`, `infer_error_segment()` -- Path visualization
- `load_filament()`, `unload_filament()`, `select_slot()`, `change_tool()` -- Operations
- `recover()`, `reset()`, `cancel()` -- Recovery
- `set_slot_info()`, `set_tool_mapping()` -- Configuration
- `enable_bypass()`, `disable_bypass()`, `is_bypass_active()` -- Bypass mode

**Optional overrides (with default implementations):**

- `reset_lane()` -- Per-lane reset (default: NOT_SUPPORTED)
- `get_dryer_info()`, `start_drying()`, `stop_drying()`, `update_drying()` -- Dryer control
- `get_endless_spool_capabilities()`, `get_endless_spool_config()`, `set_endless_spool_backup()` -- Endless spool
- `get_tool_mapping_capabilities()`, `get_tool_mapping()` -- Tool mapping
- `get_device_sections()`, `get_device_actions()`, `execute_device_action()` -- Device-specific actions
- `set_discovered_lanes()`, `set_discovered_tools()` -- Discovery configuration
- `supports_auto_heat_on_load()` -- Auto-heat capability

### 4. Wire into the Factory

In `src/printer/ams_backend.cpp`, add cases to both `create()` overloads:

```cpp
case AmsType::MY_SYSTEM:
    return std::make_unique<AmsBackendMySystem>(api, client);
```

### 5. Add Mock Support

In `src/printer/ams_backend.cpp`, extend the `HELIX_MOCK_AMS` environment variable handling:

```cpp
if (type_str == "mysystem") {
    mock->set_my_system_mode(true);
}
```

Add corresponding `set_my_system_mode()` to `AmsBackendMock` if the new system has unique UI characteristics that need simulation.

### 6. Update AmsState (if needed)

If the new backend has special discovery requirements, update `AmsState::init_backend_from_hardware()` accordingly. For example, ACE supports both object-list detection (`ace` in `printer.objects.list`) and a REST probe fallback.

### 7. Add Tests

Write tests for:
- State parsing from Moonraker JSON
- G-code command generation
- Error handling and recovery
- Tool/slot mapping
- Path segment computation

See `tests/unit/test_ams_backend_happy_hare.cpp`, `test_ams_tool_mapping.cpp`, `test_ams_endless_spool.cpp`, and `test_ams_device_actions.cpp` for patterns.

---

## Spoolman Management & Spool Wizard

Beyond slot assignment, HelixScreen provides full Spoolman spool management:

- **SpoolmanPanel overlay** — Browse, search, edit, and delete spools with virtualized list (20-row pool)
- **New Spool Wizard** — 3-step guided creation: Vendor → Filament → Spool Details
- **Context menu** — Per-spool actions: Set Active, Edit, Delete
- **Edit modal** — Update weight, price, lot number, notes via PATCH

### Spool Wizard Architecture

The wizard (`SpoolWizardOverlay`) is a 3-step overlay:

1. **Step 0 — Select Vendor**: Search/filter vendors from Spoolman server, or create a new one via modal (`create_vendor_modal.xml`)
2. **Step 1 — Select Filament**: Filter filaments by selected vendor (`vendor.id` API param), or create a new one via modal (`create_filament_modal.xml`) with material from `filament::MATERIALS[]` database, color picker, temp ranges, weight
3. **Step 2 — Spool Details**: Remaining weight, price, lot number, notes — compact 2-column layout

Key patterns:
- **Modal forms** for vendor/filament creation (not inline) — keeps list scroll area maximized
- **Vendor filtering**: Filament API uses `vendor.id=X` (Spoolman's dot-notation filter syntax)
- **Color picker**: HSV picker + preset swatches, launched from filament creation modal
- **Atomic creation**: Creates vendor → filament → spool in sequence with best-effort rollback on failure
- **Row selection**: `LV_STATE_CHECKED` with `selected_style` (primary left border + elevated bg)

### Key Files

| File | Purpose |
|------|---------|
| `include/ui_spool_wizard.h` | Wizard overlay class declaration |
| `src/ui/ui_spool_wizard.cpp` | Wizard logic, API calls, callbacks |
| `ui_xml/spool_wizard.xml` | 3-step wizard layout |
| `ui_xml/create_vendor_modal.xml` | New vendor modal form |
| `ui_xml/create_filament_modal.xml` | New filament modal form |
| `ui_xml/wizard_vendor_row.xml` | Selectable vendor row (lv_button with checked style) |
| `ui_xml/wizard_filament_row.xml` | Selectable filament row (lv_button with checked style) |
| `src/ui/ui_color_picker.cpp` | Color picker modal (used by filament creation) |

See `docs/devel/plans/2026-02-15-spool-wizard-status.md` for visual test plan.

---

## Troubleshooting

### Common Issues by Backend

#### Happy Hare

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No multi-filament system detected" | `mmu` object not in Klipper | Verify Happy Hare is installed and `[mmu]` section exists in printer.cfg |
| Gate status all "Unknown" | Subscription not receiving updates | Check Moonraker connection, verify `printer.mmu` is subscribable |
| Tool mapping not updating | Stale TTG map | Try reset tool mappings (sends 1:1 mapping for all tools) |
| Bypass button disabled | Hardware bypass sensor detected | System auto-detects bypass via sensor, manual toggle not available |

#### AFC

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No multi-filament system detected" | `AFC` object not in Klipper | Verify AFC-Klipper-Add-On is installed |
| Lane count wrong | Discovery mismatch | Check for both `AFC_stepper lane*` and `AFC_lane lane*` objects in `printer.objects.list` (OpenAMS uses `AFC_lane`) |
| Too many nozzles drawn | HUB unit map values treated as separate tools | Verify topology detection — HUB units always have tool_count=1 regardless of `map` field values |
| Hub sensors not updating | Hub name doesn't match unit name | OpenAMS uses per-lane hubs (Hub_1..Hub_N) — check hub-to-unit ownership in `unit_infos_` |
| No filament colors/materials | AFC version too old or no Spoolman | `lane_data` database requires v1.0.32+; assign spools in Spoolman |
| Device actions missing | Backend not returning sections | Verify AFC backend is connected (not mock) |
| Bowden length slider shows wrong range | Default 450mm being used | Hub data may not be received yet; wait for state sync |
| Quiet mode not toggling | G-code not recognized | Verify AFC firmware supports `AFC_QUIET_MODE` command |

#### ACE (Anycubic ACE Pro)

| Symptom | Cause | Fix |
|---------|-------|-----|
| ACE Pro not detected | Object not in list + REST probe failed | Verify a ValgACE/BunnyACE/DuckACE driver is installed; check `ace` in `printer.objects.list` and `/server/ace/info` endpoint |
| Stale state | Polling interval | ACE polls at 500ms; state may lag slightly |
| Dryer not controllable | Missing REST bridge | BunnyACE/DuckACE users must install ValgACE's `ace_status.py` Moonraker component |

#### Tool Changer

| Symptom | Cause | Fix |
|---------|-------|-----|
| No tools shown | `toolchanger` object missing | Verify klipper-toolchanger is installed |
| Wrong tool count | Discovery mismatch | Check that `tool T*` objects appear in `printer.objects.list` |
| "Uninitialized" status | Tools not homed | Run `T0` or `SELECT_TOOL TOOL=T0` to initialize |

#### AD5X IFS

| Symptom | Cause | Fix |
|---------|-------|-----|
| IFS not detected | Missing or outdated ZMOD firmware | Install ZMOD v1.7.0+ (v1.6.2 hard minimum). Verify `zmod_ifs.py` is installed and `_ifs_port_sensor_*` sensors appear in `printer.objects.list` |
| Colors/materials empty | `save_variables` not populated | Run IFS calibration wizard in ZMOD to initialize `less_waste_*` variables |
| Slots all EMPTY | Port sensors not subscribed | Check that `filament_switch_sensor _ifs_port_sensor_{1-4}` are present |
| Tool mapping wrong | Stale `less_waste_tools` | Check `save_variables.variables.less_waste_tools` — ports are 1-based, 5=unmapped |
| Bypass stuck on | `less_waste_external` = 1 | Set via ZMOD UI or `SAVE_VARIABLE VARIABLE=less_waste_external VALUE=0` |

### Debug Logging

Run with `-vv` (DEBUG) or `-vvv` (TRACE) to see backend-specific logging:

```bash
./build/bin/helix-screen --test -vv
```

All backends log with prefixes:

| Prefix | Backend |
|--------|---------|
| `[AMS Backend]` | Factory/creation |
| `[AMS Happy Hare]` / `[AmsBackendHappyHare]` | Happy Hare |
| `[AMS AFC]` | AFC |
| `[AMS ACE]` | ACE (Anycubic ACE Pro) |
| `[AMS ToolChanger]` | Tool Changer |
| `[AMS AD5X-IFS]` | AD5X IFS |
| `[AmsBackendMock]` | Mock |

### Error Result Codes

See `ams_error.h` for the full `AmsResult` enum. Key results:

| Result | Recoverable | Typical Cause |
|--------|-------------|---------------|
| `FILAMENT_JAM` | Yes | Filament stuck in path |
| `SLOT_BLOCKED` | Yes | Slot obstructed |
| `EXTRUDER_COLD` | Yes | Nozzle below load temp |
| `LOAD_FAILED` | Yes | Load did not complete |
| `UNLOAD_FAILED` | Yes | Unload did not complete |
| `BUSY` | No (wait) | Another operation in progress |
| `NOT_SUPPORTED` | No | Feature not available on this backend |
| `HOMING_FAILED` | Yes | Selector home failed |

`AmsErrorHelper` provides factory methods for creating user-friendly error messages with suggestions for each error type.

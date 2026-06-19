# Filament Slot Metadata — `lane_data` Convention

**Status**: Informational, v1.3 (2026-06). See [Changelog](#changelog).

This document describes HelixScreen's use of the `lane_data` Moonraker database
namespace to share per-slot filament metadata with OrcaSlicer and other tools.
It is published so third parties — firmware vendors, slicers, scales, spool
trackers — can interoperate with the same records without reverse-engineering
HelixScreen's source.

HelixScreen did **not** invent this convention. We document here what we write,
what we read, and the conservative rules we follow so adopters have a single
reference to implement against.

---

## 1. Overview

Moonraker exposes a per-instance JSON key/value store at
`/server/database/item`. The `lane_data` namespace, originally introduced by
[AFC (Armored Turtle Filament Changer)](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html),
holds one record per filament lane/slot. AFC writes these records from its
Klipper plugin; OrcaSlicer reads them (read-only — it never writes back) to
auto-populate filament presets when sending a print.

> **Happy Hare also writes `lane_data`.** Happy Hare's Moonraker component
> (`components/mmu_server.py`, `push_lane_data`) writes per-lane records into
> the `lane_data` namespace directly, using key names `vendor_name`, `name`,
> and `filament_id`. OrcaSlicer prefers this Moonraker `lane_data` source over
> the legacy live `mmu` Klipper status object (`GET
> /printer/objects/query?mmu`, fields
> `gate_status`/`gate_material`/`gate_color`/`gate_temperature`) when the
> namespace is populated. So Happy Hare is a `lane_data` writer alongside AFC
> and HelixScreen — its key convention (`vendor_name` / `name`) is the de-facto
> schema leader, since Orca follows it. HelixScreen emits those keys as aliases
> (see §3) for forward-compatibility.
>
> HelixScreen does **not** write `lane_data` records for AFC or Happy Hare
> backends — those plugins own their own lane records, and HelixScreen would
> only clobber them. HelixScreen writes `lane_data` only for backends that have
> no native `lane_data` writer (IFS, CFS, ACE, Snapmaker).

HelixScreen participates as both reader and writer. When a user edits slot
metadata in HelixScreen's filament panel (brand, material, color, Spoolman
binding, weight), we persist that record to `lane_data` in the AFC-compatible
shape. OrcaSlicer 2.3.2+ reads it back on the next print and pre-selects the
correct filament preset — with no HelixScreen-specific integration on the
slicer side. Verified unchanged through OrcaSlicer 2.4.0-beta: the namespace
and the consumed fields (`lane`, `color`, `material`, `bed_temp`,
`nozzle_temp`) are identical to 2.3.2.

The convention is intentionally permissive:

- No schema enforcement. Readers parse what they understand, ignore the rest.
- No locking or transactional semantics. Last writer wins.
- `scan_time` (ISO-8601 UTC) is the advisory conflict-avoidance hint.

---

## 2. The `lane_data` convention

### Origin and consumers

| Project | Role | Reference |
|---------|------|-----------|
| AFC | Originator, writes from Klipper plugin | [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html) |
| OrcaSlicer 2.3.2+ | Reader only — never writes back (filament preset auto-sync) | [MoonrakerPrinterAgent.cpp, PR #12086](https://github.com/OrcaSlicer/OrcaSlicer/pull/12086) |
| HelixScreen | Reader + writer (this document) | `src/printer/filament_slot_override_store.cpp` |

Happy Hare also writes this namespace — its Moonraker component
(`components/mmu_server.py`, `push_lane_data`) emits per-lane records keyed with
`vendor_name` / `name` / `filament_id`, and OrcaSlicer prefers that Moonraker
source over the live `mmu` Klipper object when it is populated. See the note in
§1; HelixScreen mirrors Happy Hare's key names as aliases (§3).

### Moonraker endpoint

Fetch the whole namespace:

```
GET /server/database/item?namespace=lane_data
```

Response shape:

```json
{
  "result": {
    "namespace": "lane_data",
    "value": {
      "lane1": { "lane": "0", "color": "#FF5500", "material": "PLA", ... },
      "lane2": { "lane": "1", "color": "#10A0E0", "material": "PETG", ... }
    }
  }
}
```

Per-key get / post / delete:

```
GET    /server/database/item?namespace=lane_data&key=lane1
POST   /server/database/item      # body: { namespace, key, value }
DELETE /server/database/item      # body: { namespace, key }
```

### Top-level shape

One JSON object keyed by lane identifier (`lane1`, `lane2`, …). Each value is
an object conforming to the record shape in Section 3.

Keys outside the `laneN` pattern may exist in the namespace (other tools may
have reserved the namespace for adjacent uses); readers should skip them
rather than erroring. HelixScreen iterates and only accepts keys beginning with
`lane`.

---

## 3. HelixScreen's emitted record

A full HelixScreen-emitted record looks like this:

```json
{
  "lane": "0",
  "color": "#FF5500",
  "material": "PLA",
  "vendor": "Polymaker",
  "vendor_name": "Polymaker",
  "spool_id": 42,
  "scan_time": "2026-04-18T12:34:56Z",
  "bed_temp": 60,
  "nozzle_temp": 215,

  "spool_name": "PolyLite PLA Orange",
  "name": "PolyLite PLA Orange",
  "spoolman_vendor_id": 7,
  "remaining_weight_g": 850.0,
  "total_weight_g": 1000.0,
  "color_name": "Orange"
}
```

The top group is AFC-standard. The bottom group is HelixScreen's extensions.

### Field reference

#### AFC-standard fields

| Field | Type | Required | Format / units | Semantics | Source |
|-------|------|----------|----------------|-----------|--------|
| `lane` | string | yes | stringified integer, 0-based | Tool / slot index as interpreted by the slicer. Matches OrcaSlicer's tool-index convention. See §4 for the intentional off-by-one versus the outer DB key. | HelixScreen writes slot index as string. |
| `color` | string | optional | `#RRGGBB` hex | Slot color. Leading `#` is conventional; HelixScreen's parser also accepts `0x`-prefixed forms on read. Emitted only when the override carries a non-zero RGB. | User-edited, or firmware-reported on backends where the user has no override. |
| `material` | string | optional | short code (`PLA`, `PETG`, `ABS`, `TPU`, …) | Material family. Readers should treat unknown values as opaque strings — do NOT silently map them. | User-edited. |
| `vendor` | string | optional | free-form | Brand / manufacturer. Readers match case-insensitively when pairing with their own filament databases. | User-edited. |
| `vendor_name` | string | optional | free-form | Alias of `vendor`, mirroring Happy Hare's key convention (`push_lane_data` in `components/mmu_server.py`). Emitted with the same value as `vendor` for forward-compat: as OrcaSlicer moves toward vendor-aware preset matching, `vendor_name` is the key it is most likely to consume. Zero-cost today — Orca ignores unknown keys. HelixScreen's reader accepts either key. | Same as `vendor`. |
| `spool_id` | integer | optional | positive integer | Spoolman spool ID for the physical spool currently loaded. Omitted when zero. | User-selected from Spoolman, if configured. |
| `scan_time` | string | optional | ISO-8601 UTC, second precision (`YYYY-MM-DDTHH:MM:SSZ`) | Last time this record was written or scanned. Advisory only — used for conflict avoidance, not for mutual exclusion. Sub-second fractions are truncated. | `std::chrono::system_clock::now()` at save time. |
| `bed_temp` | integer | optional | Celsius | Recommended bed temperature. | User entry, bound Spoolman spool's filament profile, or HelixScreen's internal material DB (in that priority order). |
| `nozzle_temp` | integer | optional | Celsius | Recommended nozzle temperature. When derived from a Spoolman spool's min/max range, the midpoint is emitted. | Same priority order as `bed_temp`. |

OrcaSlicer (2.3.2 through 2.4.0-beta, verified) only consumes `lane`, `color`,
`material`, `bed_temp`, and `nozzle_temp`. All other fields are additive and
must be silently ignored by compliant readers.

#### HelixScreen extension fields

These are additive and namespaced into the same record object. Other tools
reading `lane_data` should ignore any they don't understand (the "safe_json_*"
discipline that OrcaSlicer's `MoonrakerPrinterAgent` uses internally — never
throw on unknown keys).

| Field | Type | Required | Format / units | Semantics | Source |
|-------|------|----------|----------------|-----------|--------|
| `spool_name` | string | optional | free-form | Human-readable name for the spool (e.g. `"PolyLite PLA Orange"`). Distinct from `vendor` + `material` because users often want a friendlier label. | User-edited, or auto-filled from Spoolman. |
| `name` | string | optional | free-form | Alias of `spool_name`, mirroring Happy Hare's key convention (`push_lane_data` in `components/mmu_server.py`). Emitted with the same value as `spool_name` for forward-compat. HelixScreen's reader accepts either key. | Same as `spool_name`. |
| `spoolman_vendor_id` | integer | optional | positive integer | Spoolman vendor ID, paired with `spool_id` for full Spoolman round-tripping. Omitted when zero. | From Spoolman when a spool is selected. |
| `remaining_weight_g` | float | optional | grams | Remaining filament weight. Negative = unset / unknown. | Spoolman, or user-entered. |
| `total_weight_g` | float | optional | grams | Full-spool nominal weight. Negative = unset / unknown. | Spoolman, or user-entered. |
| `color_name` | string | optional | free-form | Human-readable color label (e.g. `"Orange"`), distinct from the `color` hex value. Some user workflows care about the marketing name as well as the RGB. | User-edited, or auto-filled from Spoolman. |

Fields are emitted only when present. Empty strings, zero, and negative floats
are treated as "not set" and omitted from the written record — reducing noise
and making future schema evolution easier.

---

## 4. Key mapping and indexing

There are two distinct indices for the same slot. This is intentional and
matches AFC / OrcaSlicer expectations.

| Index | Where | Base | Example for HelixScreen's slot 0 |
|-------|-------|------|----------------------------------|
| Outer DB key | `lane_data` namespace key | 1-based | `"lane1"` |
| Inner `lane` field | Record body | 0-based (stringified) | `"0"` |

The 1-based DB key matches AFC's on-disk layout (AFC labels its lanes
`lane1`, `lane2`, … in its own config too). The 0-based inner field matches
how OrcaSlicer's `MoonrakerPrinterAgent` parses it: as a tool/extruder index
starting at T0.

HelixScreen's writer produces the key via `lane_key(i) = "lane" + (i+1)` and
the inner field via `std::to_string(i)`. Readers parse the inner `lane` field
to recover the 0-based slot index and use that as the canonical identifier;
the outer key is treated as opaque once the `lane` prefix is confirmed.

Negative values in the inner `lane` field are rejected on read. This matches
`MoonrakerPrinterAgent.cpp:796`.

---

## 5. Merge policy

HelixScreen combines three sources of slot metadata:

1. **Firmware-reported state** — what the printer's AMS/IFS/CFS/AFC plugin
   thinks is loaded (read from Klipper objects or vendor REST APIs).
2. **Override records** — the `lane_data` entries written by HelixScreen or
   any other well-behaved writer.
3. **User edits in progress** — in-memory, not yet persisted.

The merge rule is **override-wins, field-by-field**:

- If an override record contains a non-empty / non-zero / non-negative field,
  it replaces the firmware-reported value for that field in the UI.
- If a field is missing, empty, zero, or negative in the override, the
  firmware value falls through.
- User edits are committed to the override record atomically on save; there is
  no partial-edit state on disk.

A tool reading these records does not need to replicate HelixScreen's merge
rule — it is documented here so third parties understand why we emit only the
fields we do, and why we omit defaulted fields rather than writing zeros.

---

## 6. Hardware-event clearing

HelixScreen automatically clears its own override records when a backend
detects that the *physical* spool in a slot has changed. This prevents stale
"user said this was orange PLA" metadata from surviving a spool swap the user
never re-entered in the UI.

Per backend:

| Backend | Signal | Rationale |
|---------|--------|-----------|
| AD5X IFS | `Adventurer5M.json` color transition to a materially different RGB | No RFID on IFS; color is the only spool-identity signal firmware exposes. |
| Snapmaker U1 | `CARD_UID` change on the RFID tag | Snapmaker tags every spool with a unique UID — the cleanest available fingerprint. |
| CFS | Per-slot composite `material_type|color_value` fingerprint change | CFS rewrites both fields from a server-side RFID lookup, so their concatenation is a stable spool fingerprint. |
| ACE | Slot status transition `EMPTY → present` | ACE has no RFID or stable tag; the only reliable "different spool" proxy is an empty-to-loaded transition. |

Clearing is a `DELETE` on the slot's `lane_data` key. The first observation
after startup establishes the baseline fingerprint and is NOT treated as a
swap — otherwise every app launch would wipe overrides.

Third-party writers do **not** need to implement this behavior. It is
documented here for transparency so readers understand why HelixScreen-authored
records may disappear between sessions.

---

## 7. Third-party adoption

### If you want to read records

Fetch the namespace in a single GET:

```
GET /server/database/item?namespace=lane_data
```

Iterate the returned object. For each value:

1. Confirm it's a JSON object.
2. Read `lane` (string or integer). Reject negative values.
3. Pull whichever fields you need (`color`, `material`, `vendor`,
   `spool_id`, `bed_temp`, `nozzle_temp`, etc.).
4. Silently ignore any keys you don't recognize — the namespace is shared
   and future schema extensions are expected.

The `scan_time` field is your conflict-avoidance signal: if you cache records
locally, compare `scan_time` to your cached copy before overwriting.

### If you want to write records

Write AFC-shaped records using `POST /server/database/item`:

```json
{
  "namespace": "lane_data",
  "key": "lane1",
  "value": {
    "lane": "0",
    "color": "#FF5500",
    "material": "PLA",
    "vendor": "Polymaker",
    "spool_id": 42,
    "scan_time": "2026-04-18T12:34:56Z",
    "bed_temp": 60,
    "nozzle_temp": 215
  }
}
```

Guidelines:

- **Always emit** `lane`. OrcaSlicer requires it.
- **Stamp** `scan_time` on every write. Use ISO-8601 UTC with second
  precision; other readers will rely on it.
- **Use the 1-based DB key and 0-based inner `lane`.** Breaking this
  correspondence silently desyncs every other reader.
- **Add extension fields freely.** Other tools will ignore them. If your
  extension becomes broadly useful, open a documentation PR here or in the
  AFC docs so the convention grows deliberately.
- **Best-effort only.** No transactions, no locking. If two writers race,
  last write wins. Use `scan_time` to avoid clobbering fresher data when
  you can.

### Conflict avoidance

The convention is cooperative, not transactional. The sharpest tool available
is `scan_time`:

```
if (remote.scan_time > my_cache.scan_time) {
    // Remote has fresher data — merge or skip.
} else {
    // Safe to overwrite.
}
```

Clock skew between the printer and your writer can defeat this; treat
`scan_time` as advisory.

---

## 8. Reference implementations

| Project | File | Role |
|---------|------|------|
| HelixScreen | `src/printer/filament_slot_override_store.cpp` | Reader + writer. `to_lane_data_record` / `from_lane_data_record` for record shape; `load_blocking` / `save_async` / `clear_async` for namespace I/O. |
| OrcaSlicer 2.3.2–2.4.0-beta | `src/slic3r/Utils/MoonrakerPrinterAgent.cpp:727-822` (`fetch_moonraker_filament_data`) | Reader (filament preset auto-sync on print send). Canonical reference for AFC-standard field semantics. Unchanged across this range. |
| AFC (Armored Turtle) | Klipper plugin | Native writer. See [AFC docs](https://www.armoredturtle.xyz/docs/afc-klipper-add-on/features.html). |
| Happy Hare | `components/mmu_server.py` (`push_lane_data`) | Native writer. Writes `lane_data` records keyed with `vendor_name` / `name` / `filament_id`. OrcaSlicer prefers this Moonraker source over the live `mmu` Klipper object (`fetch_hh_filament_info`, `MoonrakerPrinterAgent.cpp:825-950`) when the namespace is populated. HelixScreen mirrors HH's `vendor_name` / `name` key convention as aliases. |

---

## Changelog

- **v1.3 (2026-06-18)**: Corrected the Happy Hare description — HH's Moonraker
  component (`components/mmu_server.py`, `push_lane_data`) writes `lane_data`
  records directly (keys `vendor_name` / `name` / `filament_id`), and
  OrcaSlicer prefers that Moonraker source over the live `mmu` Klipper object.
  HH is now documented as a `lane_data` writer alongside AFC and HelixScreen,
  not an out-of-scope separate path. HelixScreen now emits `vendor_name` (alias
  of `vendor`) and `name` (alias of `spool_name`) to mirror HH's key
  convention for forward-compat (Orca ignores unknown keys), and its reader
  accepts either spelling. HelixScreen still does **not** write `lane_data` for
  AFC / Happy Hare backends — those plugins own their own records.
- **v1.2 (2026-06-09)**: Verified against the OrcaSlicer 2.4.0-beta source
  (`MoonrakerPrinterAgent.cpp`): the `lane_data` namespace and the five consumed
  fields are unchanged from 2.3.2, and OrcaSlicer reads the namespace read-only
  (never writes back). Corrected the Happy Hare entries — HH lanes reach
  OrcaSlicer through Orca's live `mmu`-object reader (2.4.0+), not `lane_data`.
- **v1.1 (2026-04-28)**: HelixScreen now emits `bed_temp` and `nozzle_temp`
  on every save. Source priority: explicit user entry > bound Spoolman
  spool's filament profile > internal material database default (looked up
  by `material` name at write time). For `nozzle_temp` derived from a
  Spoolman min/max range, the midpoint is emitted as a single integer to
  match the AFC wire format.
- **v1 (2026-04)**: HelixScreen's initial adoption of the `lane_data`
  convention. AFC-standard fields (`lane`, `color`, `material`, `vendor`,
  `spool_id`, `scan_time`, `bed_temp`, `nozzle_temp`) plus HelixScreen
  extensions (`spool_name`, `spoolman_vendor_id`, `remaining_weight_g`,
  `total_weight_g`, `color_name`). `bed_temp` / `nozzle_temp` documented
  but not yet emitted (lifted in v1.1).

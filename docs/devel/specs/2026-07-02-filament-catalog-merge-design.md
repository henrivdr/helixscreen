# Filament Catalog Merge — Phase 1: Data Foundation

**Status:** Design approved (pending written-spec review)
**Date:** 2026-07-02
**Author:** Preston Brown
**Scope:** Phase 1 of 3 (data layer + importer + CFS migration). Phases 2 (offline picker UI) and 3 (user editability UI) are separate specs.

---

## 1. Problem & Motivation

HelixScreen has **two** filament data sources that don't talk to each other:

1. **`include/filament_database.h`** — a header-only `constexpr` table of generic material **types** (PLA, ABS, PETG, …) with rich curated metadata (nozzle range, bed, category, drying, density, chamber, endless-spool `compat_group`). Drives all material-**type** dropdowns and lookups. No brands. Embedded-safe (baked in, no I/O, cannot fail at runtime).
2. **`assets/cfs_materials.json`** — a runtime JSON table of branded products keyed by hardware material code. Consumed at **exactly one** call site (`src/printer/ams_backend_cfs.cpp:632`, gated behind `#if HELIX_HAS_CFS`) to decode Creality Filament System box-reported codes into brand + temps. **No UI reads it.** Its 4 `P100x` "Polymaker" entries are inert placeholders (real CFS scans are numeric; a `P1001` never matches).

Consequences:
- A user **without Spoolman** can only pick a generic material *type* — never a branded product like "Polymaker ABS Pro."
- The branded catalog is hand-maintained, thin (4 Polymaker entries, all PLA), and reaches no one but CFS hardware.

**Goal:** merge these into one generic, sustainably-sourced filament catalog that (a) keeps the CFS decode path working with *richer* data, and (b) becomes the foundation for an offline branded picker (Phase 2) and user editability (Phase 3).

---

## 2. Decisions (settled during brainstorming)

| # | Decision |
|---|----------|
| Editability | Curated built-in catalog **+** user can edit built-ins **and** add their own entries (overlay store). *(UI is Phase 3; Phase 1 builds the load/merge path.)* |
| Data source | **OrcaSlicer filament library** as the primary source of branded facts. Hand-maintaining our own is a treadmill. |
| Spoolman | The offline catalog is a **fallback** — when Spoolman is connected, branded selection uses Spoolman; the catalog serves the non-Spoolman case. |
| Rename | `cfs_materials.json` → `filaments.json` (no longer CFS-specific). |
| Memory | **On-demand load, unloaded after use — never resident in RAM.** Zero idle footprint (AD5M/K1 memory diet). |
| Licensing | Temps/densities are **facts** (not copyrightable). We **derive** factual values into our own schema; we never ship Orca's AGPL profile files. Attribution to OrcaSlicer as the data source. |

---

## 3. Architecture

```
OrcaSlicer Filament Library  (AGPL — pinned checkout, dev/regen ONLY, never shipped)
        │
        │  scripts/import_orca_filaments.py   →   make regen-filaments
        │    · shallow-clone Orca at a pinned tag into scratch, run, discard
        │    · resolve `inherits` chains by profile name (deep-merge child-over-parent)
        │    · extract FACTS: vendor, type, nozzle target+range, bed, density
        │    · UNION with preserved CFS-code seed (Creality codes Orca can't supply)
        ▼
assets/filaments.json   (GENERATED, committed, OUR schema — replaces cfs_materials.json)
android/app/src/main/assets/assets/filaments.json   (mirror, regenerated in lockstep)
        │
        │  loaded ON DEMAND, dropped after use  (no resident singleton)
        ▼
FilamentCatalog  (scoped object, include/filament_catalog.h — NOT CFS-gated)
   ├─ each product resolves its base TYPE via filament::find_material(type)  ← types stay constexpr
   ├─ load_codes("cfs")  → tiny code-index slice → CFS decode (transient per box-state pass)
   ├─ load_full()        → whole catalog → offline picker (Phase 2, transient per session)
   └─ + user overlay (config/user_filaments.json, read-write) merged on load — Phase 3 authors it
```

Key structural choices:
- **Types stay in `filament_database.h`** (untouched `constexpr`, embedded-safe, source of physical truth). Orca supplies only the branded **product** layer; products carry *deltas* and inherit the rich fields from their base type.
- **Build-time import, not runtime.** Parsing thousands of inheriting Orca profiles on an AD5M/K1 is a non-starter; a `make regen-filaments` codegen step (same pattern as fonts/translations) turns the maintenance treadmill into one command and ships only a small flat catalog.

---

## 4. Data Model

### 4.1 `filaments.json` — generated branded catalog

An **array** of product objects (was a code-keyed map — RFID code is now optional, not the primary key):

```json
[
  {
    "id": "creality-hyper-pla",     // stable slug (importer-generated); user overrides target this
    "brand": "Creality",
    "name": "Hyper PLA",            // display = "{brand} {name}"
    "type": "PLA",                  // → resolves to a filament_database.h type for inheritance
    "nozzle": 220,                  // recommended (Orca nozzle_temperature)
    "bed": 60,                      // recommended (Orca plate temp, collapsed)
    "nozzle_min": 190,              // OPTIONAL — emitted only when it differs from the type's range
    "nozzle_max": 240,              // OPTIONAL
    "density": 1.24,                // optional; else inherit type
    "codes": {                      // OPTIONAL, open scheme-keyed map (§4.3)
      "cfs":  "01001",              //   Creality Filament System hardware code (preserved)
      "rfid": null                  //   reserved: future vendor-neutral RFID standard
    },
    "orca_id": "OGF...",            // provenance: Orca filament_id (NOT a CFS code)
    "source": "cfs-seed"            // provenance: orca | cfs-seed | user
  }
]
```

Most Orca-derived entries are **thin** — `id, brand, name, type, nozzle, bed`. Everything else (min/max range, chamber, dry temp/time, `compat_group`, density) **inherits from the base `type`**.

### 4.2 `FilamentCatalog` — access layer (transient, on-demand)

```
// NO ::instance(). Scoped object; constructed → queried → destroyed (frees all parsed data).
FilamentCatalog::load_codes(scheme)   // builds ONLY the code index for one scheme (small, fast)
FilamentCatalog::load_full()          // whole catalog + user overlay (picker)

// queries on a loaded instance:
resolve_code(scheme, code) -> Effective?    // CFS: resolve_code("cfs", stripped)
resolve_id(id)             -> Effective?
products_for_type(type)    -> [Effective]
products_for_brand(brand)  -> [Effective]
all_brands() / all_products()

// indices built on load: by_id · by_brand · by_type · by_code[scheme]
```

`Effective` is the fully-resolved view: `type defaults (find_material(type)) ◀ product fields ◀ user overrides`. So **one call** yields recommended nozzle + safe min/max + bed + chamber + dry + `compat_group` — for both CFS decode and the picker.

**Memory discipline (mandatory):**
- No resident singleton. Idle footprint = **0**.
- `load_codes("cfs")` materializes only the coded slice (~66 entries), transiently, for one CFS box-state enrichment pass, then is destroyed.
- `load_full()` materializes the whole catalog only while a picker session is open (Phase 2), then is destroyed.
- On-disk `filaments.json` is a shipped asset; only its parsed form is transient.
- Tradeoff (accepted): repeated small re-parse on frequent CFS polls in exchange for zero idle RAM. Future escape hatch if profiling demands it: a short debounce cache. **Default is no residency.**

### 4.3 `codes` — open scheme-keyed tag map

RFID/hardware codes live in a scheme-keyed map so multiple, possibly-colliding namespaces coexist and new ones drop in with **no schema change**:

| scheme | meaning | Phase 1 status |
|--------|---------|----------------|
| `cfs` | Creality Filament System numeric hardware code | **populated** from the preserved seed |
| `rfid` | future vendor-neutral / generic RFID standard | reserved (null) |
| `snapmaker` | Snapmaker U1 `filament_sku` (e.g. `900001`) | reserved — feasible, see §7 |
| `bambu` | Bambu `filament_id`/RFID (`GFA00`…) | reserved — could auto-fill from `orca_id` later |

`by_code` is indexed **per scheme**, so a `cfs` "01001" can never collide with a generic `rfid` number. CFS decode is `resolve_code("cfs", …)`; a future generic reader is `resolve_code("rfid", …)` with no structural change.

---

## 5. The Importer (`scripts/import_orca_filaments.py`)

### 5.1 Two inputs (a union, not a replacement)

```
INPUT 1  OrcaFilamentLibrary (pinned)   → broad branded facts, NO Creality codes
INPUT 2  the current numeric-coded CFS entries (Creality/Generic/eSUN, ~66 of the 77)
         → carried forward verbatim; these ARE the CFS hardware decode table (Orca has no equivalent)
OUTPUT   filaments.json  =  Orca-derived products  ∪  preserved CFS-code seed
```

Only the 4 fake `P100x` Polymaker placeholders are dropped, replaced by real Orca-derived Polymaker products (correctly with no `code`).

### 5.2 Field mapping (verified against SoftFever/OrcaSlicer `main`)

| our field | Orca source |
|---|---|
| `type` | `filament_type` → mapping table (Orca 75-value open enum → our types; unknown → raw string fallback) |
| `brand` | `filament_vendor[0]` |
| `name` | derived from profile `name` (strip `@System`/`@<printer>`, brand prefix) |
| `nozzle` | `nozzle_temperature[0]` |
| `nozzle_min` / `nozzle_max` | `nozzle_temperature_range_low[0]` / `nozzle_temperature_range_high[0]` — emitted **only** when they differ from the base type's range |
| `bed` | collapse Orca's 5 plate temps → one: **first real** (non-`nil`, non-zero) value in `textured_plate_temp` → `hot_plate_temp` → `cool_plate_temp`; if none resolve, inherit the base type's `bed_temp`. (`hot` = high-temp catch-all — engineering/GF/CF; `cool` = low-temp — glacier/etc. `eng` and `supertack` intentionally ignored.) |
| `density` | `filament_density[0]` |
| `orca_id` | `filament_id` (provenance; **not** a CFS code) |
| `id` | slug from `brand` + product `name` |

### 5.3 Mechanics (from verified schema)

- Every Orca value is a per-extruder **string array** → take `[0]`, parse to number; treat sentinel `"nil"` as inherit/unset.
- Resolve `inherits` **by profile `name`** up the chain (typically `product @base → fdm_filament_<type> → fdm_filament_common`), deep-merging child-over-parent. Base profiles (`instantiation:false`) are templates, not products.
- **Scope resolution to `OrcaFilamentLibrary/filament/` ONLY.** The library is self-contained: it ships its own canonical bases under `OrcaFilamentLibrary/filament/base/fdm_filament_*.json`, and every library profile's `inherits` chain resolves within that subtree (verified: 0 missing targets). **Do NOT load the ~30 other vendor packs** — each ships its own same-named `fdm_filament_*` (e.g. 24 copies of `fdm_filament_pc`), and a flat name→profile map lets whichever loaded last clobber the library's base, corrupting the inherited vendor + temperatures (a library "Generic PC" wrongly inheriting a vendor's PC base → wrong brand/temps, mostly lint-invisible). This was a real bug; the importer keys resolution to the library subtree and a regression test (`scripts/fixtures/orca_collision/`) guards it.
- **Scope:** the ~482 `OrcaFilamentLibrary/` profiles (28 `Generic … @System` + per-brand), **not** the ~6,550 printer-specific ones. Collapse per-color leaves to one product (dedup by `orca_id` / brand+product).
- **Determinism:** stable sort + canonical formatting → clean regen diffs.

### 5.4 Sourcing (licensing-clean)

`make regen-filaments` shallow-clones OrcaSlicer at a **pinned tag** into scratch, runs the importer, emits `filaments.json` + the Android mirror, discards the clone. **Nothing AGPL is committed to our tree** — we commit only our derived factual catalog, with an attribution note naming OrcaSlicer + the pinned tag. Bump the tag to refresh.

---

## 6. CFS Migration, Rename & Plumbing

- **Decode swap:** `ams_backend_cfs.cpp:632` `db.lookup(mat_id)` → `FilamentCatalog::load_codes("cfs").resolve_code("cfs", mat_id)`, loaded once at the top of the box-state enrichment pass (decode all slots) and destroyed at pass end. `strip_code()` (the `6-digit-starting-with-1 → 5-digit` rule) stays in the CFS backend — it's hardware-protocol normalization, not catalog logic.
- **Parity:** CFS fills the same slot fields it does today (`slot.material/brand/nozzle_temp_min/max`). Extra resolved metadata (bed/chamber/dry) is available but only wired where the slot struct already carries it — **no behavior change**, just no longer leaving data on the floor.
- **`CfsMaterialDb` is deleted**; its JSON-loading role moves into `FilamentCatalog` (non-gated). Unrelated static helpers in that file (`slot_to_tnn`, `parse_color`, `strip_code`, …) stay.
- **Rename** `cfs_materials.json` → `filaments.json`: 3 loader search paths (moved into `filament_catalog.cpp`, now also probing `config/filaments.json` + read-write `config/user_filaments.json`), `Makefile:1083-1084` + `mk/cross.mk:2441-2445` (`RELEASE_ASSET_FILES`), and the Android mirror.
- **Migration:** none needed — the CFS catalog was never a user-facing store, so replacing the shipped asset is clean. `user_filaments.json` is new and empty by default. (Caveat: an undocumented on-device hand-edit of `cfs_materials.json` is not carried over — acceptable; editability now has a proper home in Phase 3.)

---

## 7. Snapmaker U1 tags (reserved, out of scope for Phase 1)

The U1 **does** expose a stable per-material product code — `print_task_config.filament_sku` (e.g. `900001`) — the true CFS-code analog. It fits as `codes.snapmaker` with **no schema change**. But it's a **follow-on integration**, not Phase 1, because:
1. We don't parse the SKU today (intentionally skipped — `ams_backend_snapmaker.cpp:713`, asserted in tests). Wiring it means reading `filament_sku` into slot identity + `resolve_code("snapmaker", sku)`.
2. No SKU→product seed data exists yet (Snapmaker SKUs are vendor-specific; Orca has none). Populating `codes.snapmaker` needs a hand-collected SKU table (like the CFS seed), built from U1 captures we don't have.

Notes for whoever does it: use `print_task_config.filament_sku` (populated regardless of RFID state), **not** `filament_detect.info[].SKU` (returns `0`/`"NONE"` when the U1's RFID reader is disabled — the default on Extended Firmware). `CARD_UID` is a per-*spool* fingerprint (wrong granularity for a catalog key) — leave it in swap detection.

Phase 1 deliverable here: `codes` is the open scheme-map, so `snapmaker` (and `rfid`, `bambu`) are valid future keys.

---

## 8. Testing

- **Importer** (Python, pinned-fixture golden test): inheritance-chain resolution; `[0]` / `"nil"` extraction; field mapping (nozzle target+range, bed-plate collapse, density); type table (known→mapped, unknown→raw fallback); color dedup; **deterministic output** (byte-identical on re-run).
- **`FilamentCatalog`** (`tests/unit/test_filament_catalog.cpp`, Catch2 / `HelixTestFixture`): `resolve_code("cfs","01001")` hit/miss; `Effective` inheritance (thin product inherits type range/bed/chamber/dry; explicit override wins); user-overlay merge (override-by-id + add-new; absent overlay → built-in only); **scheme isolation** (`cfs` code can't collide with `rfid`); `load_codes` builds only the coded slice (footprint check).
- **CFS parity** (existing `tests/unit/test_ams_backend_cfs.cpp`): the critical "don't break CFS" guard — decode fills the same slot fields after the `CfsMaterialDb`→`FilamentCatalog` swap.
- **Data lint** (bats or small C++ check): every shipped `type` resolves in `filament_database.h` (or is a known fallback); no duplicate `id`s; no duplicate codes within a scheme; `min ≤ nozzle ≤ max`; Android-mirror byte-identity.

---

## 9. Rollout Sequence

1. `filament_database.h` — **untouched**.
2. Add importer + generated `filaments.json` + `make regen-filaments`; commit the generated file (generated-but-committed, like fonts/translations — cross-compile needs it present).
3. Add `FilamentCatalog` (non-gated, scoped/transient) + tests.
4. Migrate CFS backend → `FilamentCatalog`; delete `CfsMaterialDb`; rename file + update the 3 loader paths + Makefile/cross.mk packaging + Android mirror.
5. Verify: `make test-run` (catalog + CFS parity) + a `pi-docker` cross build (confirms packaging paths).
6. **No user-facing change** lands in Phase 1.

---

## 10. Risks

| Risk | Mitigation |
|------|-----------|
| **AGPL entanglement** (Orca is AGPL-3.0, we're GPL-3.0-or-later) | Derive **facts** (temps/densities aren't copyrightable), never ship Orca files; attribute. Owner (Preston) has accepted this stance; residual "curated selection" gray area is bounded to factual specs + attribution. |
| Importer inheritance-resolution bugs | Golden-fixture tests on real chains; deterministic output makes regressions diff-visible. |
| Bed-plate collapse heuristic imperfect per material | First-real-value `textured → hot → cool` (hot = high-temp catch-all, cool = low-temp); base type `bed_temp` backstop when all plates are `nil`, so a bad profile never ships a wrong bed. |
| Type-mapping table drift (Orca open enum) | Defensive fallback to raw string; table is small and reviewed at each regen. |
| CFS re-parse I/O on frequent polls | Accepted for zero idle RAM; debounce cache is a future escape hatch only if profiling shows cost. |

---

## 11. Out of Scope (future phases)

- **Phase 2 — Offline picker UI:** branded-filament selection for non-Spoolman users, wired to the filament-panel preset buttons (and/or AMS edit modal / spool wizard), sourced from `FilamentCatalog`, active as a Spoolman fallback.
  - **Design constraint (MUST): no flat all-products list.** The catalog is 356 products / 21 brands / 35 types, and **PLA alone is 168** (one brand has 46 PLA products) — a single scrollable list is unusable.
  - **Target shape: a 2-stage modal — vendor → product — with the material *type* pre-scoped by the launching preset button** (so it's 2 taps, not vendor→type→product). Stage 1 lists vendors (incl. "Generic"); Stage 2 lists that vendor's products *in that type*.
  - **Stage 1 MUST be filtered to vendors that actually carry the selected type** (`products_for_type()` + `brand`), so niche types (PA-CF, PC) show a short vendor list. Pin "Generic" + recently-used/favorites on top; show a per-vendor product-count badge. The only long view is a big brand's PLA (~46) — acceptable single-vendor scroll, add an in-stage filter later if needed.
  - Open interaction detail for the Phase-2 brainstorm: whether picking a product updates the preset's displayed type; how brand+temps are written to the slot via `EffectiveFilament`; the entry point when a type has no preset button.
- **Phase 3 — User editability UI:** edit built-ins + add custom entries; the read-write `config/user_filaments.json` overlay authored via UI. (Phase 1 already builds the load/merge path.)
- **Snapmaker U1 `codes.snapmaker` decode** (§7) — parse `filament_sku` + build the SKU seed table.
- **Bambu tag decode** (`codes.bambu` from `orca_id`).

---

## 12. Attribution

`filaments.json` carries a header note: factual filament data derived from the **OrcaSlicer** filament library (`github.com/SoftFever/OrcaSlicer`, pinned tag `<TAG>`, AGPL-3.0), used as factual reference. HelixScreen ships no OrcaSlicer profile files.

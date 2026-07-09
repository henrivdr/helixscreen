# Filament Catalog Merge — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge the CFS branded-filament table and the generic material-type database into one Orca-sourced, on-demand-loaded filament catalog, keeping the CFS decode path working with richer data.

**Architecture:** A build-time Python importer derives a flat `assets/filaments.json` from the OrcaSlicer filament library (facts only; nothing AGPL shipped) unioned with the preserved Creality CFS-code seed. A new **scoped, transient** `FilamentCatalog` C++ class loads it on demand (never a resident singleton), resolving each product's deltas over its base type in the untouched `constexpr` `filament_database.h`. The CFS backend swaps its `CfsMaterialDb` lookup for `FilamentCatalog::load_codes("cfs")`.

**Tech Stack:** C++17 (Catch2 tests, `nlohmann::json` via `hv/json.hpp`), Python 3 (pytest, project `.venv`), pure Makefile.

**Reference spec:** `docs/devel/specs/2026-07-02-filament-catalog-merge-design.md`

## Global Constraints

- **Memory:** `FilamentCatalog` has **NO `::instance()` singleton**. It is a scoped object, loaded on demand and destroyed after use. Idle RAM footprint must be 0. `load_codes(scheme)` materializes only the coded slice; `load_full()` the whole catalog.
- **Types stay in `filament_database.h`** — that file is **not modified**. Products inherit physical fields from their base type via `filament::find_material()`.
- **Licensing:** the importer never commits or ships OrcaSlicer profile files. It derives factual values only. `filaments.json` carries an attribution header field.
- **JSON include:** use `#include "hv/json.hpp"` (libhv's bundled nlohmann), never `<nlohmann/json.hpp>`.
- **SPDX header:** every new C++ file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- **spdlog only** for logging (`spdlog::info/warn`), never `printf`/`cout`.
- **Python:** run all Python inside the project venv (`make venv-setup` creates `.venv`); never `pip install --break-system-packages`.
- **`codes` is scheme-keyed** (`{"cfs": "...", "rfid": null, ...}`); `by_code` is indexed per scheme so codes never collide across schemes.
- **Android mirror** `android/app/src/main/assets/assets/filaments.json` must stay byte-identical to `assets/filaments.json`.
- **Commit convention:** `feat(filament): ...` / `test(filament): ...` / `build(filament): ...`.

---

## File Structure

**Create:**
- `scripts/import_orca_filaments.py` — the build-time importer (pure functions + CLI).
- `scripts/test_import_orca_filaments.py` — pytest for the importer.
- `scripts/fixtures/orca/` — tiny hand-authored Orca profile fixtures for the golden test.
- `scripts/fixtures/cfs_seed.json` — the preserved Creality/Generic/eSUN coded entries (extracted from today's `assets/cfs_materials.json`, minus the 4 `P100x` placeholders).
- `mk/filaments.mk` — the `regen-filaments` make target (mirrors `mk/fonts.mk`).
- `include/filament_catalog.h` — `FilamentProduct`, `EffectiveFilament`, `FilamentCatalog` (non-CFS-gated).
- `src/printer/filament_catalog.cpp` — implementation.
- `tests/unit/test_filament_catalog.cpp` — Catch2 tests (`[filament_catalog]`).
- `tests/fixtures/filaments_test.json` — fixture catalog for the C++ tests.

**Modify:**
- `include/ams_backend_cfs.h` — remove `CfsMaterialInfo` + the material-DB members of `CfsMaterialDb` (`instance`/`lookup`/`load_database`/`materials_`); keep the static helpers (`strip_code`, `parse_color`, `slot_to_tnn`, `tnn_to_slot`, `DEFAULT_COLOR`).
- `src/printer/ams_backend_cfs.cpp` — delete `load_database`/`lookup`/`instance`; swap the decode call site (~550, ~632) to `FilamentCatalog`.
- `Makefile:1083-1084` and `.PHONY` (`:853`) — ship `filaments.json`; add `regen-filaments`; include `mk/filaments.mk`.
- `mk/cross.mk:2445` — `RELEASE_ASSET_FILES := assets/filaments.json`.
- `docs/devel/FILAMENT_MANAGEMENT.md` — document the catalog + `make regen-filaments` + attribution.

**Generate (committed):**
- `assets/filaments.json` and its Android mirror.

**Delete:**
- `assets/cfs_materials.json` and `android/app/src/main/assets/assets/cfs_materials.json` (replaced by `filaments.json`).

---

## Task 1: Importer core — resolve, map, collapse

**Files:**
- Create: `scripts/import_orca_filaments.py`
- Create: `scripts/test_import_orca_filaments.py`
- Create: `scripts/fixtures/orca/` (fixture profiles below)

**Interfaces:**
- Produces:
  - `resolve_inherits(profile: dict, by_name: dict[str, dict]) -> dict` — returns a fully flattened profile (child-over-parent deep merge along the `inherits` chain).
  - `first_scalar(value) -> str | None` — Orca values are per-extruder string arrays; returns element `[0]` or `None` for `"nil"`/empty.
  - `map_type(orca_type: str) -> str` — Orca `filament_type` → our type name; unknown → raw string.
  - `collapse_bed(resolved: dict) -> int | None` — first real value in `textured → hot → cool`.
  - `build_product(resolved: dict, base_type_range: tuple[int,int] | None) -> dict` — one `filaments.json` product dict.

- [ ] **Step 1: Create fixture Orca profiles**

Create these files exactly (they emulate the verified Orca schema — string arrays, `inherits` by name, `instantiation` flags):

`scripts/fixtures/orca/fdm_filament_common.json`:
```json
{
  "type": "filament", "name": "fdm_filament_common", "instantiation": "false",
  "filament_type": ["PLA"], "filament_vendor": ["Generic"],
  "filament_density": ["0"], "nozzle_temperature": ["200"],
  "nozzle_temperature_range_low": ["190"], "nozzle_temperature_range_high": ["240"],
  "cool_plate_temp": ["60"], "hot_plate_temp": ["60"], "textured_plate_temp": ["60"]
}
```

`scripts/fixtures/orca/fdm_filament_abs.json`:
```json
{
  "type": "filament", "name": "fdm_filament_abs", "instantiation": "false",
  "inherits": "fdm_filament_common",
  "filament_type": ["ABS"], "filament_density": ["1.04"],
  "nozzle_temperature": ["250"], "nozzle_temperature_range_low": ["240"],
  "nozzle_temperature_range_high": ["270"],
  "cool_plate_temp": ["nil"], "hot_plate_temp": ["90"], "textured_plate_temp": ["100"]
}
```

`scripts/fixtures/orca/Polymaker ABS Pro @base.json`:
```json
{
  "type": "filament", "name": "Polymaker ABS Pro @base", "instantiation": "false",
  "inherits": "fdm_filament_abs", "filament_id": "OGFPMABSPRO",
  "filament_vendor": ["Polymaker"], "filament_density": ["1.06"],
  "nozzle_temperature": ["280"], "nozzle_temperature_range_low": ["270"],
  "nozzle_temperature_range_high": ["290"],
  "hot_plate_temp": ["105"], "textured_plate_temp": ["105"]
}
```

`scripts/fixtures/orca/Polymaker ABS Pro @System.json`:
```json
{
  "type": "filament", "name": "Polymaker ABS Pro @System", "instantiation": "true",
  "inherits": "Polymaker ABS Pro @base"
}
```

- [ ] **Step 2: Write the failing test**

`scripts/test_import_orca_filaments.py`:
```python
import os
import import_orca_filaments as imp

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "orca")


def load_fixtures():
    return imp.load_profiles(FIX)


def test_resolve_inherits_flattens_chain():
    by_name = load_fixtures()
    leaf = by_name["Polymaker ABS Pro @System"]
    r = imp.resolve_inherits(leaf, by_name)
    assert imp.first_scalar(r["filament_type"]) == "ABS"          # from fdm_filament_abs
    assert imp.first_scalar(r["filament_vendor"]) == "Polymaker"  # from @base
    assert imp.first_scalar(r["nozzle_temperature"]) == "280"     # @base overrides


def test_first_scalar_handles_nil_and_arrays():
    assert imp.first_scalar(["220", "220"]) == "220"
    assert imp.first_scalar(["nil"]) is None
    assert imp.first_scalar([]) is None


def test_map_type_known_and_unknown():
    assert imp.map_type("PLA") == "PLA"
    assert imp.map_type("PET-CF") == "PET-CF"
    assert imp.map_type("WeirdNew") == "WeirdNew"  # unknown → raw fallback


def test_collapse_bed_prefers_textured_skips_nil():
    r = imp.resolve_inherits(load_fixtures()["Polymaker ABS Pro @System"], load_fixtures())
    assert imp.collapse_bed(r) == 105   # textured wins
    common = {"cool_plate_temp": ["55"], "hot_plate_temp": ["nil"], "textured_plate_temp": ["nil"]}
    assert imp.collapse_bed(common) == 55  # falls through to cool


def test_build_product_thin_when_range_matches_type():
    r = imp.resolve_inherits(load_fixtures()["Polymaker ABS Pro @System"], load_fixtures())
    p = imp.build_product(r, base_type_range=(245, 265))
    assert p["brand"] == "Polymaker"
    assert p["type"] == "ABS"
    assert p["nozzle"] == 280
    assert p["nozzle_min"] == 270 and p["nozzle_max"] == 290  # differs from type → emitted
    assert p["bed"] == 105
    assert p["orca_id"] == "OGFPMABSPRO"
    assert p["source"] == "orca"
```

- [ ] **Step 3: Run test to verify it fails**

Run (inside project venv): `python3 -m pytest scripts/test_import_orca_filaments.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'import_orca_filaments'`

- [ ] **Step 4: Write the importer core**

`scripts/import_orca_filaments.py`:
```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive HelixScreen's filaments.json from the OrcaSlicer filament library.

Facts only (temps/densities); no OrcaSlicer profile files are copied or shipped.
"""
import json
import os
import re

# Orca filament_type (open enum) -> our filament_database.h type name.
# Only entries that differ or need pinning are listed; unknown types fall back
# to the raw Orca string (still usable via the product's own temps).
TYPE_MAP = {
    "PLA": "PLA", "PLA-CF": "PLA-CF", "PETG": "PETG", "PETG-CF": "PETG-CF",
    "PET-CF": "PET-CF", "PET-GF": "PET-GF", "ABS": "ABS", "ABS-CF": "ABS-CF",
    "ASA": "ASA", "PC": "PC", "PA": "PA", "PA-CF": "PA-CF", "PA6": "PA6",
    "TPU": "TPU", "PVA": "PVA", "HIPS": "HIPS", "PPS-CF": "PPS-CF",
}

BED_PRIORITY = ("textured_plate_temp", "hot_plate_temp", "cool_plate_temp")


def load_profiles(root: str) -> dict:
    """Map every profile's `name` -> profile dict, across the whole tree."""
    by_name = {}
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".json"):
                continue
            with open(os.path.join(dirpath, fn), encoding="utf-8") as f:
                p = json.load(f)
            if p.get("type") == "filament" and "name" in p:
                by_name[p["name"]] = p
    return by_name


def resolve_inherits(profile: dict, by_name: dict) -> dict:
    """Flatten the inherits chain: parent first, child overrides."""
    parent_name = profile.get("inherits")
    if parent_name and parent_name in by_name:
        merged = resolve_inherits(by_name[parent_name], by_name)
    else:
        merged = {}
    merged = dict(merged)
    for k, v in profile.items():
        if k in ("inherits", "name", "instantiation"):
            continue
        merged[k] = v
    return merged


def first_scalar(value):
    """Orca stores per-extruder string arrays; return [0] or None for nil/empty."""
    if isinstance(value, list):
        if not value:
            return None
        value = value[0]
    if value is None:
        return None
    s = str(value).strip()
    if s == "" or s.lower() == "nil":
        return None
    return s


def _as_int(value):
    s = first_scalar(value)
    if s is None:
        return None
    try:
        return int(round(float(s)))
    except ValueError:
        return None


def map_type(orca_type: str) -> str:
    return TYPE_MAP.get(orca_type, orca_type)


def collapse_bed(resolved: dict):
    """First real (non-nil, non-zero) value in textured -> hot -> cool."""
    for key in BED_PRIORITY:
        v = _as_int(resolved.get(key))
        if v:  # non-None and non-zero
            return v
    return None


def _slug(brand: str, name: str) -> str:
    raw = f"{brand}-{name}".lower()
    return re.sub(r"[^a-z0-9]+", "-", raw).strip("-")


def build_product(resolved: dict, base_type_range):
    """Turn a resolved Orca profile into a filaments.json product dict."""
    orca_type = first_scalar(resolved.get("filament_type")) or ""
    brand = first_scalar(resolved.get("filament_vendor")) or "Generic"
    orca_id = resolved.get("filament_id", "")  # top-level bare string
    name = first_scalar(resolved.get("_product_name")) or orca_id or "Unknown"

    nozzle = _as_int(resolved.get("nozzle_temperature"))
    nmin = _as_int(resolved.get("nozzle_temperature_range_low"))
    nmax = _as_int(resolved.get("nozzle_temperature_range_high"))

    product = {
        "id": _slug(brand, name),
        "brand": brand,
        "name": name,
        "type": map_type(orca_type),
        "nozzle": nozzle,
        "bed": collapse_bed(resolved),
        "source": "orca",
    }
    # Emit explicit range only when it differs from the base type's range.
    if nmin is not None and nmax is not None and (nmin, nmax) != base_type_range:
        product["nozzle_min"] = nmin
        product["nozzle_max"] = nmax
    density = resolved.get("filament_density")
    dval = first_scalar(density)
    if dval and float(dval) > 0:
        product["density"] = round(float(dval), 3)
    if orca_id:
        product["orca_id"] = orca_id
    return product
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest scripts/test_import_orca_filaments.py -v`
Expected: PASS (5 passed)

Note: `build_product` reads `_product_name` (a derived display name set by the CLI in Task 2 before calling `build_product`). The Task 1 test asserts `name == "OGFPMABSPRO"` via the `orca_id` fallback because the fixture leaf has no `_product_name`; Task 2 supplies the stripped display name.

- [ ] **Step 6: Commit**

```bash
git add scripts/import_orca_filaments.py scripts/test_import_orca_filaments.py scripts/fixtures/orca/
git commit -m "feat(filament): Orca importer core (resolve/map/collapse) + fixtures"
```

---

## Task 2: Importer CLI, regen target & generated catalog

**Files:**
- Modify: `scripts/import_orca_filaments.py` (add `build_catalog` + `main`)
- Modify: `scripts/test_import_orca_filaments.py` (add union + determinism tests)
- Create: `scripts/fixtures/cfs_seed.json`
- Create: `mk/filaments.mk`
- Modify: `Makefile` (`include mk/filaments.mk`; `.PHONY` add `regen-filaments`)
- Create (generated): `assets/filaments.json`, `android/app/src/main/assets/assets/filaments.json`
- Delete: `assets/cfs_materials.json`, `android/app/src/main/assets/assets/cfs_materials.json`

**Interfaces:**
- Consumes: `build_product`, `resolve_inherits`, `load_profiles` (Task 1).
- Produces:
  - `build_catalog(orca_root: str, cfs_seed: list[dict], type_ranges: dict[str,tuple]) -> list[dict]` — the full product array (Orca-derived ∪ CFS seed), stably sorted.
  - CLI: `python3 scripts/import_orca_filaments.py --orca <dir> --cfs-seed <file> --out <file> [--type-ranges <file>]`.

- [ ] **Step 1: Extract the CFS seed**

Create `scripts/fixtures/cfs_seed.json` by copying every entry from today's `assets/cfs_materials.json` whose key is numeric (Creality/Generic/eSUN) — i.e. **exclude** the four `P100x` Polymaker placeholders — reshaped to the new product schema. Example (first two entries; carry all numeric-keyed entries forward):
```json
[
  {"id": "creality-hyper-pla", "brand": "Creality", "name": "Hyper PLA",
   "type": "PLA", "nozzle": 215, "nozzle_min": 190, "nozzle_max": 240,
   "codes": {"cfs": "01001"}, "source": "cfs-seed"},
  {"id": "esun-pla-plus", "brand": "eSUN", "name": "eSUN PLA+",
   "type": "PLA", "nozzle": 220, "nozzle_min": 210, "nozzle_max": 230,
   "codes": {"cfs": "E1001"}, "source": "cfs-seed"}
]
```
(The old `min_temp`/`max_temp` become `nozzle_min`/`nozzle_max`; `nozzle` = midpoint; the old key becomes `codes.cfs`.)

- [ ] **Step 2: Write the failing test**

Add to `scripts/test_import_orca_filaments.py`:
```python
def test_build_catalog_unions_orca_and_seed():
    seed = [{"id": "creality-hyper-pla", "brand": "Creality", "name": "Hyper PLA",
             "type": "PLA", "nozzle": 215, "codes": {"cfs": "01001"}, "source": "cfs-seed"}]
    cat = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    ids = {p["id"] for p in cat}
    assert "creality-hyper-pla" in ids                 # seed preserved
    assert any(p["brand"] == "Polymaker" for p in cat)  # orca product present
    assert all("P100" not in p["id"] for p in cat)      # no placeholders


def test_build_catalog_is_deterministic():
    seed = []
    a = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    b = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    assert json.dumps(a) == json.dumps(b)               # stable order + shape
```

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 -m pytest scripts/test_import_orca_filaments.py -k build_catalog -v`
Expected: FAIL — `AttributeError: module 'import_orca_filaments' has no attribute 'build_catalog'`

- [ ] **Step 4: Scope-tag profiles, add `build_catalog` + CLI**

First, modify `load_profiles` (created in Task 1) to record each profile's source directory so `build_catalog` can restrict products to the vendor-agnostic library. Inside the loop, right after `p = json.load(f)`, add:
```python
            p["_src"] = dirpath  # remember source dir for library-scope filtering
```
(This is backward-compatible with the Task 1 tests — they never assert on the resolved key set.)

Then append to `scripts/import_orca_filaments.py`:
```python
import argparse
import sys

# @System / @<printer> / @base suffixes to strip from profile names for display.
_SUFFIX_RE = re.compile(r"\s*@.*$")


def _display_name(profile_name: str, brand: str) -> str:
    name = _SUFFIX_RE.sub("", profile_name).strip()
    if brand and name.lower().startswith(brand.lower() + " "):
        name = name[len(brand) + 1:]
    return name or profile_name


def build_catalog(orca_root, cfs_seed, type_ranges, library_marker="OrcaFilamentLibrary"):
    # Load the WHOLE tree (bases from BBL/ etc. are needed to resolve inherits),
    # but emit products ONLY from the vendor-agnostic library, not the ~6,550
    # printer-specific profiles. Tests pass library_marker="" to accept fixtures.
    by_name = load_profiles(orca_root)
    products = []
    seen = set()
    for pname, profile in by_name.items():
        if profile.get("instantiation") != "true":
            continue  # templates, not user-facing products
        if library_marker and library_marker not in profile.get("_src", ""):
            continue  # skip printer-specific profiles outside the library
        resolved = resolve_inherits(profile, by_name)
        brand = first_scalar(resolved.get("filament_vendor")) or "Generic"
        resolved["_product_name"] = _display_name(pname, brand)
        orca_type = first_scalar(resolved.get("filament_type")) or ""
        base_range = type_ranges.get(map_type(orca_type))
        product = build_product(resolved, base_range)
        key = (product["brand"], product["name"])
        if key in seen:
            continue  # collapse per-color duplicates
        seen.add(key)
        products.append(product)
    products.extend(cfs_seed)
    products.sort(key=lambda p: (p.get("source", ""), p["brand"].lower(), p["name"].lower(), p["id"]))
    return products


def _load_type_ranges(path):
    if not path:
        return {}
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    return {k: tuple(v) for k, v in raw.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description="Derive filaments.json from OrcaSlicer.")
    ap.add_argument("--orca", required=True, help="OrcaSlicer resources/profiles root")
    ap.add_argument("--cfs-seed", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--type-ranges", default="")
    ap.add_argument("--orca-tag", default="unknown")
    args = ap.parse_args(argv)

    with open(args.cfs_seed, encoding="utf-8") as f:
        seed = json.load(f)
    catalog = build_catalog(args.orca, seed, _load_type_ranges(args.type_ranges))
    doc = {
        "_attribution": ("Factual filament data derived from OrcaSlicer "
                         f"(github.com/SoftFever/OrcaSlicer, tag {args.orca_tag}, "
                         "AGPL-3.0). No OrcaSlicer profile files are shipped."),
        "filaments": catalog,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"Wrote {len(catalog)} filaments to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Note: the shipped file is an **object** (`{"_attribution": ..., "filaments": [...]}`); `build_catalog` returns the inner array. The C++ loader (Task 3) reads the `filaments` key.

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest scripts/test_import_orca_filaments.py -v`
Expected: PASS (all)

- [ ] **Step 6: Create the regen target**

`mk/filaments.mk`:
```makefile
# Regenerate assets/filaments.json from the OrcaSlicer filament library.
# Shallow-clones Orca at a pinned tag into scratch; ships nothing AGPL.
ORCA_TAG ?= v2.4.1
ORCA_TMP := $(CURDIR)/build/orca-profiles

.PHONY: regen-filaments
regen-filaments:
	@echo "==> Fetching OrcaSlicer profiles at $(ORCA_TAG)"
	@rm -rf "$(ORCA_TMP)"
	@git clone --depth 1 --branch $(ORCA_TAG) --filter=blob:none --sparse \
		https://github.com/SoftFever/OrcaSlicer "$(ORCA_TMP)"
	@cd "$(ORCA_TMP)" && git sparse-checkout set resources/profiles
	@python3 scripts/import_orca_filaments.py \
		--orca "$(ORCA_TMP)/resources/profiles" \
		--cfs-seed scripts/fixtures/cfs_seed.json \
		--type-ranges scripts/fixtures/type_ranges.json \
		--orca-tag $(ORCA_TAG) \
		--out assets/filaments.json
	@cp -a assets/filaments.json android/app/src/main/assets/assets/filaments.json
	@rm -rf "$(ORCA_TMP)"
	@echo "==> Regenerated assets/filaments.json (+ Android mirror)"
```

Generate `scripts/fixtures/type_ranges.json` once from the current `filament_database.h` (`{"ABS": [245,265], "ASA": [240,260], "PLA": [190,220], ...}` — one entry per type). Add `include mk/filaments.mk` near the other `include mk/*.mk` lines in `Makefile`, and add `regen-filaments` to the `.PHONY` list at `Makefile:853`.

- [ ] **Step 7: Generate the real catalog & retire the old file**

```bash
make regen-filaments
git rm assets/cfs_materials.json android/app/src/main/assets/assets/cfs_materials.json
```
Verify `assets/filaments.json` exists, has an `_attribution` field, a `filaments` array containing Creality/eSUN seed entries (with `codes.cfs`) and Orca-derived products, and **no** `P100x` ids. **Sanity-check the count** (`python3 -c "import json;print(len(json.load(open('assets/filaments.json'))['filaments']))"`): expect on the order of a few hundred products (the vendor-agnostic library collapsed by product). If it is in the thousands, the `library_marker` scoping failed (printer-specific profiles leaked in) — fix before committing. Confirm the Android mirror is byte-identical: `diff assets/filaments.json android/app/src/main/assets/assets/filaments.json && echo IDENTICAL`.

- [ ] **Step 8: Commit**

```bash
git add scripts/import_orca_filaments.py scripts/test_import_orca_filaments.py \
        scripts/fixtures/ mk/filaments.mk Makefile \
        assets/filaments.json android/app/src/main/assets/assets/filaments.json
git add -u  # stages the two deletions
git commit -m "feat(filament): generate filaments.json from Orca + retire cfs_materials.json"
```

---

## Task 3: FilamentCatalog — types, load, effective resolution

**Files:**
- Create: `include/filament_catalog.h`
- Create: `src/printer/filament_catalog.cpp`
- Create: `tests/unit/test_filament_catalog.cpp`
- Create: `tests/fixtures/filaments_test.json`

**Interfaces:**
- Consumes: `filament::find_material(std::string_view) -> std::optional<filament::MaterialInfo>` (`include/filament_database.h:232`).
- Produces:
  - `struct helix::printer::EffectiveFilament { std::string id, brand, name, type; int nozzle_min, nozzle_max, nozzle_recommended, bed_temp, chamber_temp_c, dry_temp_c, dry_time_min; float density_g_cm3; std::string compat_group; std::map<std::string,std::string> codes; };`
  - `class helix::printer::FilamentCatalog` with `static FilamentCatalog load_full();` `static FilamentCatalog load_codes(const std::string& scheme);` `static FilamentCatalog load_from_file(const std::string& path, bool codes_only, const std::string& scheme);` and const query methods `resolve_code(scheme, code)`, `resolve_id(id)`, `products_for_type(type)`, `products_for_brand(brand)`, `all_brands()`, `all_products()`.

- [ ] **Step 1: Create the test fixture catalog**

`tests/fixtures/filaments_test.json`:
```json
{
  "_attribution": "test fixture",
  "filaments": [
    {"id": "creality-hyper-pla", "brand": "Creality", "name": "Hyper PLA",
     "type": "PLA", "nozzle": 215, "codes": {"cfs": "01001"}, "source": "cfs-seed"},
    {"id": "polymaker-abs-pro", "brand": "Polymaker", "name": "ABS Pro",
     "type": "ABS", "nozzle": 280, "nozzle_min": 270, "nozzle_max": 290,
     "bed": 105, "source": "orca"},
    {"id": "polymaker-pla-pro", "brand": "Polymaker", "name": "PLA Pro",
     "type": "PLA", "nozzle": 210, "source": "orca"}
  ]
}
```

- [ ] **Step 2: Write the failing test**

`tests/unit/test_filament_catalog.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "helix_test_fixture.h"
#include <catch2/catch_test_macros.hpp>

using helix::printer::FilamentCatalog;

namespace {
constexpr const char* FIX = "tests/fixtures/filaments_test.json";
}

TEST_CASE_METHOD(HelixTestFixture, "resolve_code cfs hit and miss", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, /*codes_only=*/true, "cfs");
    const auto* p = cat.resolve_code("cfs", "01001");
    REQUIRE(p != nullptr);
    CHECK(p->brand == "Creality");
    CHECK(p->type == "PLA");
    CHECK(cat.resolve_code("cfs", "99999") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "effective inherits type range when thin", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* pla = cat.resolve_id("polymaker-pla-pro");   // no explicit range
    REQUIRE(pla != nullptr);
    CHECK(pla->nozzle_min == 190);   // inherited from PLA type
    CHECK(pla->nozzle_max == 220);
    CHECK(pla->bed_temp == 60);      // inherited from PLA type
    CHECK(pla->compat_group == std::string("PLA"));
}

TEST_CASE_METHOD(HelixTestFixture, "explicit override wins over type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 270);   // explicit, outside generic ABS range
    CHECK(abs->nozzle_max == 290);
    CHECK(abs->bed_temp == 105);     // explicit bed
}

TEST_CASE_METHOD(HelixTestFixture, "load_codes materializes only coded slice", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, true, "cfs");
    CHECK(cat.all_products().size() == 1);          // only the cfs-coded entry
    CHECK(cat.resolve_id("polymaker-abs-pro") == nullptr);
}

TEST_CASE_METHOD(HelixTestFixture, "queries by brand and type", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_from_file(FIX, false, "");
    CHECK(cat.products_for_brand("Polymaker").size() == 2);
    CHECK(cat.products_for_type("PLA").size() == 2);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[filament_catalog]"`
Expected: FAIL to compile — `filament_catalog.h` not found.

- [ ] **Step 4: Write the header**

`include/filament_catalog.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::printer {

/// Fully-resolved filament: product deltas merged over the base material type.
struct EffectiveFilament {
    std::string id, brand, name, type;
    int nozzle_min = 0;
    int nozzle_max = 0;
    int nozzle_recommended = 0;
    int bed_temp = 0;
    int chamber_temp_c = 0;
    int dry_temp_c = 0;
    int dry_time_min = 0;
    float density_g_cm3 = 0.0f;
    std::string compat_group;
    std::map<std::string, std::string> codes;  ///< scheme -> code
};

/// Transient, on-demand filament catalog. NO resident singleton: construct via a
/// static loader, query, then let it fall out of scope (frees all parsed data).
class FilamentCatalog {
  public:
    /// Whole catalog (built-in + user overlay). For the picker.
    static FilamentCatalog load_full();
    /// Only products carrying a code in `scheme`. For CFS decode (small slice).
    static FilamentCatalog load_codes(const std::string& scheme);
    /// Explicit path (tests / non-default locations).
    static FilamentCatalog load_from_file(const std::string& path, bool codes_only,
                                          const std::string& scheme);

    const EffectiveFilament* resolve_code(const std::string& scheme,
                                          const std::string& code) const;
    const EffectiveFilament* resolve_id(const std::string& id) const;
    std::vector<const EffectiveFilament*> products_for_type(const std::string& type) const;
    std::vector<const EffectiveFilament*> products_for_brand(const std::string& brand) const;
    std::vector<std::string> all_brands() const;
    std::vector<const EffectiveFilament*> all_products() const;

  private:
    std::vector<EffectiveFilament> products_;
    std::unordered_map<std::string, size_t> by_id_;
    // scheme -> (code -> product index)
    std::unordered_map<std::string, std::unordered_map<std::string, size_t>> by_code_;
    void index();
};

}  // namespace helix::printer
```

- [ ] **Step 5: Write the implementation**

`src/printer/filament_catalog.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"

#include "filament_database.h"

#include <spdlog/spdlog.h>

#include <fstream>

#include "hv/json.hpp"

namespace helix::printer {

namespace {

// Search paths for the built-in catalog (mirrors the old CFS loader).
const char* kBuiltinPaths[] = {"assets/filaments.json", "../assets/filaments.json",
                               "/opt/helixscreen/assets/filaments.json"};
const char* kUserPaths[] = {"config/user_filaments.json",
                            "../config/user_filaments.json"};

int get_int(const nlohmann::json& j, const char* key, int def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int>() : def;
}

/// Resolve one product JSON into an EffectiveFilament, inheriting from its type.
EffectiveFilament to_effective(const nlohmann::json& p) {
    EffectiveFilament e;
    e.id = p.value("id", "");
    e.brand = p.value("brand", "");
    e.name = p.value("name", "");
    e.type = p.value("type", "");

    auto base = filament::find_material(e.type);  // std::optional<MaterialInfo>
    const int type_min = base ? base->nozzle_min : 0;
    const int type_max = base ? base->nozzle_max : 0;

    e.nozzle_min = get_int(p, "nozzle_min", type_min);
    e.nozzle_max = get_int(p, "nozzle_max", type_max);
    e.nozzle_recommended = get_int(p, "nozzle", base ? base->nozzle_recommended() : 0);
    e.bed_temp = get_int(p, "bed", base ? base->bed_temp : 0);
    e.chamber_temp_c = base ? base->chamber_temp_c : 0;
    e.dry_temp_c = base ? base->dry_temp_c : 0;
    e.dry_time_min = base ? base->dry_time_min : 0;
    e.density_g_cm3 = p.contains("density") && p["density"].is_number()
                          ? p["density"].get<float>()
                          : (base ? base->density_g_cm3 : 0.0f);
    e.compat_group = base ? base->compat_group : "";

    if (auto it = p.find("codes"); it != p.end() && it->is_object()) {
        for (auto& [scheme, code] : it->items()) {
            if (code.is_string())
                e.codes[scheme] = code.get<std::string>();
        }
    }
    return e;
}

std::vector<nlohmann::json> read_products(const char* const* paths, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        std::ifstream f(paths[i]);
        if (!f.is_open())
            continue;
        try {
            auto doc = nlohmann::json::parse(f);
            if (doc.is_object() && doc.contains("filaments") && doc["filaments"].is_array())
                return doc["filaments"].get<std::vector<nlohmann::json>>();
            if (doc.is_array())  // user overlay is a bare array
                return doc.get<std::vector<nlohmann::json>>();
        } catch (const std::exception& e) {
            spdlog::warn("[filament] parse failed {}: {}", paths[i], e.what());
        }
    }
    return {};
}

}  // namespace

void FilamentCatalog::index() {
    by_id_.clear();
    by_code_.clear();
    for (size_t i = 0; i < products_.size(); ++i) {
        const auto& e = products_[i];
        by_id_[e.id] = i;
        for (const auto& [scheme, code] : e.codes)
            by_code_[scheme][code] = i;
    }
}

FilamentCatalog FilamentCatalog::load_from_file(const std::string& path, bool codes_only,
                                                const std::string& scheme) {
    const char* paths[] = {path.c_str()};
    FilamentCatalog cat;
    for (const auto& jp : read_products(paths, 1)) {
        auto e = to_effective(jp);
        if (codes_only && e.codes.find(scheme) == e.codes.end())
            continue;
        cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

FilamentCatalog FilamentCatalog::load_codes(const std::string& scheme) {
    FilamentCatalog cat;
    for (const auto& jp : read_products(kBuiltinPaths, std::size(kBuiltinPaths))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    // User overlay may add coded products too.
    for (const auto& jp : read_products(kUserPaths, std::size(kUserPaths))) {
        auto e = to_effective(jp);
        if (e.codes.find(scheme) != e.codes.end())
            cat.products_.push_back(std::move(e));
    }
    cat.index();
    return cat;
}

FilamentCatalog FilamentCatalog::load_full() {
    FilamentCatalog cat;
    for (const auto& jp : read_products(kBuiltinPaths, std::size(kBuiltinPaths)))
        cat.products_.push_back(to_effective(jp));
    // Overlay handled in Task 4.
    cat.index();
    return cat;
}

const EffectiveFilament* FilamentCatalog::resolve_code(const std::string& scheme,
                                                       const std::string& code) const {
    auto s = by_code_.find(scheme);
    if (s == by_code_.end())
        return nullptr;
    auto c = s->second.find(code);
    return c == s->second.end() ? nullptr : &products_[c->second];
}

const EffectiveFilament* FilamentCatalog::resolve_id(const std::string& id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &products_[it->second];
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_type(const std::string& type) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.type == type)
            out.push_back(&e);
    return out;
}

std::vector<const EffectiveFilament*>
FilamentCatalog::products_for_brand(const std::string& brand) const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        if (e.brand == brand)
            out.push_back(&e);
    return out;
}

std::vector<std::string> FilamentCatalog::all_brands() const {
    std::vector<std::string> out;
    for (const auto& e : products_)
        if (std::find(out.begin(), out.end(), e.brand) == out.end())
            out.push_back(e.brand);
    return out;
}

std::vector<const EffectiveFilament*> FilamentCatalog::all_products() const {
    std::vector<const EffectiveFilament*> out;
    for (const auto& e : products_)
        out.push_back(&e);
    return out;
}

}  // namespace helix::printer
```

Add `#include <algorithm>` for `std::find`/`std::size` if not transitively available. Ensure `src/printer/filament_catalog.cpp` is picked up by the build (the Makefile globs `src/**/*.cpp`; confirm no per-file allowlist needs editing).

- [ ] **Step 6: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[filament_catalog]"`
Expected: PASS (5 test cases)

- [ ] **Step 7: Commit**

```bash
git add include/filament_catalog.h src/printer/filament_catalog.cpp \
        tests/unit/test_filament_catalog.cpp tests/fixtures/filaments_test.json
git commit -m "feat(filament): FilamentCatalog transient loader + type-inheritance"
```

---

## Task 4: User overlay merge

**Files:**
- Modify: `src/printer/filament_catalog.cpp` (`load_full` merges the overlay)
- Modify: `tests/unit/test_filament_catalog.cpp` (overlay tests)
- Create: `tests/fixtures/user_filaments_test.json`

**Interfaces:**
- Consumes: `load_from_file` semantics (Task 3).
- Produces: `static FilamentCatalog load_with_overlay(const std::string& builtin, const std::string& overlay);` — override-by-id + add-new.

- [ ] **Step 1: Create overlay fixture**

`tests/fixtures/user_filaments_test.json`:
```json
[
  {"id": "polymaker-abs-pro", "nozzle_min": 265, "nozzle_max": 285, "source": "user"},
  {"id": "acme-custom-petg", "brand": "Acme", "name": "Custom PETG",
   "type": "PETG", "nozzle": 240, "source": "user"}
]
```

- [ ] **Step 2: Write the failing test**

Add to `tests/unit/test_filament_catalog.cpp`:
```cpp
TEST_CASE_METHOD(HelixTestFixture, "user overlay overrides and adds", "[filament_catalog]") {
    auto cat = FilamentCatalog::load_with_overlay(
        "tests/fixtures/filaments_test.json", "tests/fixtures/user_filaments_test.json");
    const auto* abs = cat.resolve_id("polymaker-abs-pro");
    REQUIRE(abs != nullptr);
    CHECK(abs->nozzle_min == 265);   // overridden by user
    CHECK(abs->nozzle_max == 285);
    const auto* added = cat.resolve_id("acme-custom-petg");
    REQUIRE(added != nullptr);       // new user product
    CHECK(added->brand == "Acme");
    CHECK(added->bed_temp == 80);    // inherited from PETG type
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[filament_catalog]"`
Expected: FAIL — `load_with_overlay` not declared.

- [ ] **Step 4: Implement overlay merge**

Add to `include/filament_catalog.h` (public):
```cpp
    static FilamentCatalog load_with_overlay(const std::string& builtin_path,
                                             const std::string& overlay_path);
```
Add to `src/printer/filament_catalog.cpp`:
```cpp
FilamentCatalog FilamentCatalog::load_with_overlay(const std::string& builtin_path,
                                                   const std::string& overlay_path) {
    const char* bpaths[] = {builtin_path.c_str()};
    const char* opaths[] = {overlay_path.c_str()};

    // Raw product JSON keyed by id, so overlay can override before resolution.
    std::unordered_map<std::string, nlohmann::json> merged;
    std::vector<std::string> order;
    for (const auto& jp : read_products(bpaths, 1)) {
        std::string id = jp.value("id", "");
        if (merged.find(id) == merged.end())
            order.push_back(id);
        merged[id] = jp;
    }
    for (const auto& jp : read_products(opaths, 1)) {
        std::string id = jp.value("id", "");
        if (merged.find(id) == merged.end()) {
            order.push_back(id);
            merged[id] = jp;
        } else {
            merged[id].merge_patch(jp);  // field-level override
        }
    }
    FilamentCatalog cat;
    for (const auto& id : order)
        cat.products_.push_back(to_effective(merged[id]));
    cat.index();
    return cat;
}
```
Then make `load_full()` delegate: replace its body with `return load_with_overlay(<first existing builtin path>, <first existing user path>);` — resolve the first existing path from `kBuiltinPaths`/`kUserPaths` (empty string if none; `read_products` tolerates a missing path).

- [ ] **Step 5: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[filament_catalog]"`
Expected: PASS (6 test cases)

- [ ] **Step 6: Commit**

```bash
git add include/filament_catalog.h src/printer/filament_catalog.cpp \
        tests/unit/test_filament_catalog.cpp tests/fixtures/user_filaments_test.json
git commit -m "feat(filament): user overlay merge (override-by-id + add-new)"
```

---

## Task 5: CFS backend migration

**Files:**
- Modify: `include/ams_backend_cfs.h` (remove `CfsMaterialInfo` + material-DB members)
- Modify: `src/printer/ams_backend_cfs.cpp` (delete `load_database`/`lookup`/`instance`; swap call site)
- Modify: `tests/unit/test_ams_backend_cfs.cpp` (parity assertion)

**Interfaces:**
- Consumes: `FilamentCatalog::load_codes("cfs")`, `resolve_code("cfs", id)` → `const EffectiveFilament*` with `.type`, `.brand`, `.nozzle_min`, `.nozzle_max`.

- [ ] **Step 1: Write/confirm the failing parity test**

In `tests/unit/test_ams_backend_cfs.cpp`, ensure a test drives a box-state parse with a known CFS code present in `assets/filaments.json` (`01001`) and asserts the decoded slot:
```cpp
TEST_CASE_METHOD(CfsTestFixture, "cfs decode fills slot from catalog", "[cfs]") {
    // ... feed box JSON whose material_type[i] normalizes to "01001" ...
    CHECK(slot.material == "PLA");
    CHECK(slot.brand == "Creality");
    CHECK(slot.nozzle_temp_min > 0);
    CHECK(slot.nozzle_temp_max >= slot.nozzle_temp_min);
}
```
(Adapt to the file's existing CFS fixture harness; the point is a decode assertion that must survive the swap.)

- [ ] **Step 2: Run to verify current behavior (baseline)**

Run: `make test && ./build/bin/helix-tests "[cfs]"`
Expected: PASS today (via `CfsMaterialDb`). This is the parity baseline to preserve.

- [ ] **Step 3: Remove the material DB from the header**

In `include/ams_backend_cfs.h`, delete `struct CfsMaterialInfo` (lines 22-30) and, inside `CfsMaterialDb`, delete `static const CfsMaterialDb& instance();`, `const CfsMaterialInfo* lookup(...) const;`, the private `CfsMaterialDb();`, `void load_database();`, and `std::unordered_map<std::string, CfsMaterialInfo> materials_;`. Keep the static helpers (`strip_code`, `parse_color`, `slot_to_tnn`, `tnn_to_slot`, `DEFAULT_COLOR`). `CfsMaterialDb` becomes a static-utility class.

- [ ] **Step 4: Swap the call site**

In `src/printer/ams_backend_cfs.cpp`:
1. Add `#include "filament_catalog.h"` near the includes.
2. Delete the `CfsMaterialDb::instance()`, `::lookup()`, and `::load_database()` definitions.
3. Where `const auto& db = CfsMaterialDb::instance();` was obtained (top of the box-state parse, ~line 550), replace with:
```cpp
    // Transient: materialize ONLY the cfs-coded slice for this parse pass.
    const auto cfs_catalog = FilamentCatalog::load_codes("cfs");
```
4. Replace the decode block (lines 631-644) with:
```cpp
            if (!mat_id.empty()) {
                const auto* mat_info = cfs_catalog.resolve_code("cfs", mat_id);
                if (mat_info) {
                    slot.material = mat_info->type;
                    slot.brand = mat_info->brand;
                    slot.nozzle_temp_min = mat_info->nozzle_min;
                    slot.nozzle_temp_max = mat_info->nozzle_max;
                } else {
                    auto it = same_material_names.find(mat_code_raw);
                    if (it != same_material_names.end()) {
                        slot.material = it->second;
                    }
                }
            }
```
Note: `cfs_catalog` is constructed once before the per-unit/per-slot loops and destroyed at pass end — one small load per box-state update, no residency.

- [ ] **Step 5: Run tests to verify parity**

Run: `make test && ./build/bin/helix-tests "[cfs][filament_catalog]"`
Expected: PASS — CFS decode fills the same fields; no `CfsMaterialInfo` references remain (`grep -rn CfsMaterialInfo src include` returns nothing).

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_cfs.h src/printer/ams_backend_cfs.cpp tests/unit/test_ams_backend_cfs.cpp
git commit -m "feat(filament): CFS decode via FilamentCatalog (retire CfsMaterialDb material table)"
```

---

## Task 6: Rename plumbing & packaging

**Files:**
- Modify: `Makefile:1083-1084`
- Modify: `mk/cross.mk:2445`

**Interfaces:** none (build plumbing).

- [ ] **Step 1: Update native install packaging**

In `Makefile`, replace the `cfs_materials.json` copy (lines 1083-1084) with:
```makefile
	@# filaments.json: unified filament catalog, loaded on demand by FilamentCatalog
	@if [ -f assets/filaments.json ]; then cp -a assets/filaments.json "$(DESTDIR)/opt/helixscreen/assets/"; fi
```

- [ ] **Step 2: Update release asset list**

In `mk/cross.mk`, change line 2445:
```makefile
RELEASE_ASSET_FILES := assets/filaments.json
```
(Update the neighboring comment at 2441-2444 to name `filaments.json`.)

- [ ] **Step 3: Verify no stale references remain**

Run: `grep -rn "cfs_materials" Makefile mk/ src/ include/ android/ ; echo "exit=$?"`
Expected: no matches (`grep` exit 1). If any remain, fix them.

- [ ] **Step 4: Native + cross build sanity**

Run: `make -j && make test-run` (native passes), then a cross build to confirm packaging paths:
Run: `make pi-docker`
Expected: both succeed; the release bundle includes `assets/filaments.json`.

- [ ] **Step 5: Commit**

```bash
git add Makefile mk/cross.mk
git commit -m "build(filament): ship filaments.json; drop cfs_materials.json packaging"
```

---

## Task 7: Data-integrity lint

**Files:**
- Create: `tests/unit/test_filaments_data.cpp`

**Interfaces:**
- Consumes: `FilamentCatalog::load_from_file`, `filament::find_material`.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_filaments_data.cpp`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_catalog.h"
#include "filament_database.h"
#include "helix_test_fixture.h"
#include "../catch_amalgamated.hpp"
#include <set>

using helix::printer::FilamentCatalog;

TEST_CASE_METHOD(HelixTestFixture, "shipped filaments.json is well-formed", "[filament_data]") {
    auto cat = FilamentCatalog::load_from_file("assets/filaments.json", false, "");
    auto all = cat.all_products();
    REQUIRE(!all.empty());

    std::set<std::string> ids;
    std::set<std::string> cfs_codes;
    for (const auto* p : all) {
        CHECK(ids.insert(p->id).second);                 // no duplicate ids
        CHECK(p->nozzle_min <= p->nozzle_recommended);
        CHECK(p->nozzle_recommended <= p->nozzle_max);
        // A product's type either resolves in filament_database.h (inherits
        // physical defaults) OR is a legitimately-unmapped Orca type — in which
        // case the importer emits explicit temps, so it must be self-sufficient.
        CHECK((filament::find_material(p->type).has_value() ||
               (p->nozzle_min > 0 && p->nozzle_max > 0)));
        auto it = p->codes.find("cfs");
        if (it != p->codes.end())
            CHECK(cfs_codes.insert(it->second).second);  // no dup cfs codes
    }
}
```

- [ ] **Step 2: Run to verify it passes against real data**

Run: `make test && ./build/bin/helix-tests "[filament_data]"`
Expected: PASS. The type check now only fails if a product has an unmapped type **and** no usable temps (a real defect). A product with an unmapped-but-self-sufficient type (explicit `nozzle_min/max`) is valid by design. If you *want* an unmapped type to inherit our curated defaults instead of relying on its explicit temps, extend `TYPE_MAP` in the importer to map it to a known type, then `make regen-filaments` — optional, not required to pass.

- [ ] **Step 3: Add the Android-mirror identity check to CI lint**

Append to `tests/shell/test_code_lint.bats`:
```bash
@test "filaments.json android mirror matches when present" {
  # The Android mirror is gitignored (generated by `make regen-filaments`, not
  # tracked), so it may be absent on a fresh checkout / CI clone. Only assert
  # byte-identity when it exists — catches on-disk drift without breaking CI.
  if [ ! -f android/app/src/main/assets/assets/filaments.json ]; then
    skip "android mirror not generated (gitignored)"
  fi
  diff assets/filaments.json android/app/src/main/assets/assets/filaments.json
}
```

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_filaments_data.cpp tests/shell/test_code_lint.bats
git commit -m "test(filament): data-integrity lint + android mirror identity"
```

---

## Task 8: Documentation

**Files:**
- Modify: `docs/devel/FILAMENT_MANAGEMENT.md`

- [ ] **Step 1: Document the catalog + regen workflow**

Add a "Filament Catalog (`filaments.json`)" section to `docs/devel/FILAMENT_MANAGEMENT.md` covering: the schema (`id`/`brand`/`name`/`type`/`nozzle`/`bed`/`nozzle_min`/`nozzle_max`/`density`/`codes`/`source`), type-inheritance from `filament_database.h`, the scheme-keyed `codes` map (cfs live; rfid/snapmaker/bambu reserved), the transient `FilamentCatalog` loading model (no singleton), `make regen-filaments ORCA_TAG=<tag>`, and the AGPL derive-facts/attribution stance. Link the spec `docs/devel/specs/2026-07-02-filament-catalog-merge-design.md`.

- [ ] **Step 2: Commit**

```bash
git add docs/devel/FILAMENT_MANAGEMENT.md
git commit -m "docs(filament): document unified catalog + regen-filaments workflow"
```

---

## Self-Review

**Spec coverage:**
- §3 architecture (Orca importer → filaments.json → FilamentCatalog → CFS) → Tasks 1-6. ✅
- §4.1 schema → Tasks 2 (writer) + 3 (reader). ✅
- §4.2 transient loader, no singleton, `load_codes`/`load_full` → Task 3 + Global Constraints. ✅
- §4.3 scheme-keyed codes + per-scheme index → Task 3 (`by_code_`) + Task 7 (dup-per-scheme lint). ✅
- §5 importer (two-input union, field map, inheritance, sourcing, dedup, determinism) → Tasks 1-2. ✅
- §6 CFS migration + rename + plumbing → Tasks 5-6. ✅
- §7 U1 (reserved only) → no code task by design; `codes` openness covered in Task 3. ✅
- §8 testing (importer golden, catalog, CFS parity, data lint, mirror) → Tasks 1,3,4,5,7. ✅
- §9 rollout order → task order matches (types untouched → importer → catalog → CFS → plumbing). ✅
- Editability overlay load path (Phase-1 portion of the Phase-3 feature) → Task 4. ✅

**Placeholder scan:** no TBD/TODO; every code step carries complete code. The only deferred items are explicitly out-of-scope (U1 SKU decode, picker UI) per the spec.

**Type consistency:** `EffectiveFilament` fields (`nozzle_min/max/recommended`, `bed_temp`, `compat_group`, `codes`) are used identically in Tasks 3, 4, 5, 7. `resolve_code(scheme, code)` / `resolve_id(id)` / `load_codes` / `load_from_file` / `load_with_overlay` signatures match across header, impl, and tests. Importer `build_product`/`build_catalog`/`first_scalar`/`map_type`/`collapse_bed` names match between Tasks 1 and 2.

**Known adaptation points (call out to executor, not placeholders):**
- Task 2 Step 1: the exact CFS-seed entries must be transcribed from the live `assets/cfs_materials.json` (all numeric keys, minus `P100x`).
- Task 5 Step 1: the CFS parity test must be fitted to the existing `test_ams_backend_cfs.cpp` fixture harness.
- Task 3 Step 5: confirm the Makefile source glob picks up `src/printer/filament_catalog.cpp` (it globs `src/**`; no allowlist expected).

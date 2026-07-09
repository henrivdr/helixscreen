#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive HelixScreen's filaments.json from the OrcaSlicer filament library.

Facts only (temps/densities); no OrcaSlicer profile files are copied or shipped.
"""
import argparse
import json
import os
import re
import sys

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
            p["_src"] = dirpath  # remember source dir for library-scope filtering
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
    raw = raw.replace("+", "-plus-")  # preserve meaningful suffix, e.g. PLA+ vs PLA
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
    # Reconcile so nozzle_min <= nozzle <= nozzle_max ALWAYS. Orca's
    # nozzle_temperature is the real print temp; range_low/high are soft hints
    # that some upstream profiles set inconsistently (recommended outside range),
    # and unmapped types have no base range at all.
    lo, hi = nmin, nmax
    if base_type_range is not None:
        lo = base_type_range[0] if lo is None else lo
        hi = base_type_range[1] if hi is None else hi
    if nozzle is not None:
        lo = nozzle if lo is None else min(lo, nozzle)
        hi = nozzle if hi is None else max(hi, nozzle)
    # Emit an explicit range unless it exactly matches the base type range (thin).
    if lo is not None and hi is not None and (lo, hi) != base_type_range:
        product["nozzle_min"] = lo
        product["nozzle_max"] = hi
    density = resolved.get("filament_density")
    dval = first_scalar(density)
    if dval and float(dval) > 0:
        product["density"] = round(float(dval), 3)
    if orca_id:
        product["orca_id"] = orca_id
    return product


# @System / @<printer> / @base suffixes to strip from profile names for display.
_SUFFIX_RE = re.compile(r"\s*@.*$")


def _display_name(profile_name: str, brand: str) -> str:
    name = _SUFFIX_RE.sub("", profile_name).strip()
    if brand and name.lower().startswith(brand.lower() + " "):
        name = name[len(brand) + 1:]
    return name or profile_name


def build_catalog(orca_root, cfs_seed, type_ranges, library_marker="OrcaFilamentLibrary"):
    # The OrcaFilamentLibrary is self-contained (its own base/ dir). Resolve
    # inheritance ONLY within it: loading the ~30 other vendor packs clobbers
    # same-named bases (e.g. fdm_filament_pc) in the flat name->profile map and
    # corrupts inherited vendor + temperature values. See the collision test.
    lib_root = os.path.join(orca_root, "OrcaFilamentLibrary", "filament")
    load_root = lib_root if os.path.isdir(lib_root) else orca_root
    by_name = load_profiles(load_root)
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
    _merge_cfs_seed(products, cfs_seed)
    _dedupe_ids(products)
    products.sort(key=lambda p: (p.get("source", ""), p["brand"].lower(), p["name"].lower(), p["id"]))
    return products


def _merge_cfs_seed(products, cfs_seed):
    """Fold the CFS seed into the Orca products instead of appending it wholesale.

    The seed exists to carry per-material CFS codes (used to match a physical CFS
    spool by its embedded code), but it names its generics "Generic TPU" while the
    Orca library names the same material "TPU". Appended as-is, the two surface as
    duplicate picker rows with conflicting temps (the seed generics are nozzle-only).

    So: normalize each seed entry's display name to Orca convention (strip a leading
    brand prefix, e.g. "Generic TPU" -> "TPU"), then look for an Orca product of the
    same (brand, type, name). If found, move the seed's codes onto that richer Orca
    product (Orca temps win) and drop the seed copy. Seed entries with no Orca match
    (CFS-only variants like "Generic TPU 64D", or Creality "CR-*") are kept as
    standalone products, still carrying their codes.
    """
    def _name_key(s):
        return re.sub(r"[^a-z0-9]+", "", s.lower())  # fold "PLA-Silk" == "PLA Silk"

    orca_by_key = {(p["brand"], p.get("type"), _name_key(p["name"])): p for p in products}
    for entry in cfs_seed:
        merged = dict(entry)
        merged["name"] = _display_name(entry.get("name", ""), entry.get("brand", ""))
        merged["id"] = _slug(entry.get("brand", ""), merged["name"])
        match = orca_by_key.get((entry.get("brand"), entry.get("type"), _name_key(merged["name"])))
        if match is not None:
            codes = match.setdefault("codes", {})
            for scheme, code in (entry.get("codes") or {}).items():
                codes.setdefault(scheme, code)  # don't clobber an existing Orca code
        else:
            products.append(merged)


def _dedupe_ids(products):
    """Disambiguate duplicate `id`s in place with a `-2`, `-3`, ... suffix.

    Safety net for the union of Orca-derived products and cfs_seed: no matter
    the source, a duplicate id would silently shadow an earlier product in any
    id-keyed consumer. Logs a warning naming every colliding id.
    """
    seen_ids = {}
    collided = set()
    for product in products:
        base_id = product["id"]
        count = seen_ids.get(base_id, 0)
        if count:
            collided.add(base_id)
            new_id = f"{base_id}-{count + 1}"
            while new_id in seen_ids:
                count += 1
                new_id = f"{base_id}-{count + 1}"
            product["id"] = new_id
            seen_ids[new_id] = 1
        seen_ids[base_id] = count + 1
    if collided:
        print(f"WARNING: duplicate filament ids disambiguated: {sorted(collided)}",
              file=sys.stderr)


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

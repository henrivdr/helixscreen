# Filament Catalog Merge — Phase 2: Offline Branded Picker

**Status:** Design (brainstormed 2026-07-03). Successor to Phase 1 (`2026-07-02-filament-catalog-merge-design.md`, merged `0af05a112`).

---

## 1. Problem & Motivation

Phase 1 shipped the data foundation: `assets/filaments.json` (356 branded products) and
`FilamentCatalog` (transient, on-demand access layer, `include/filament_catalog.h`). Nothing
in the UI reads it yet — only CFS RFID-code decode consumes it.

Phase 2 makes the catalog usable: let a **non-Spoolman** user pick a specific branded
filament (e.g. "Bambu Lab / PLA / PLA Matte") and have its brand + product-specific
temperatures flow into (a) an AMS slot's metadata and (b) a filament-panel preset button.

The catalog is the **offline fallback** for branded selection (spec-1 §2). When Spoolman is
connected, the online spool picker (`spoolman_edit_modal.xml`) still owns branded selection;
the catalog serves the Spoolman-less case. The two are orthogonal controls — this phase does
not touch the Spoolman flow.

---

## 2. Decisions (settled during brainstorming)

| # | Decision |
|---|----------|
| Widget | A new modal (`Modal` / `ui_dialog`), **not** `ContextMenu`. A single stacked popup list (`MaterialPickerMenu`) cannot express two *linked* selectors. |
| Layout | **Vendor → Type** linked dropdowns driving a live product list. Vendor-first (user's call): with both dropdowns always visible there is no tap-efficiency reason to lead with type; "I have Bambu filament → narrow down" reads better. This overrides spec-1 §11's type-outer ordering. |
| Primary target | **AMS slot metadata.** The picker upgrades the existing type-only material selector in the AMS edit modal. |
| Preset target | The preset long-press opens the *same* picker; it **subsumes** the type-only `MaterialPickerMenu`. |
| Picker contract | The picker emits an `EffectiveFilament` and knows nothing about slots or presets. Each caller decides what to do with it. |
| Preset persistence | Denormalize temps at pick time (no catalog load on preset tap — preserves the zero-idle-RAM design). |
| Dead code | Delete the orphaned `filament_preset_edit_modal.xml`. Its manual name+temps editing is Phase 3 (user editability) territory. |
| Staging | **2a** = catalog API + picker modal + AMS integration (standalone). **2b** = preset upgrade + persistence migration (depends on 2a). |

---

## 3. Architecture

```
                       FilamentCatalog::load_full()   (transient, dropped on modal close)
                                   │
                                   ▼
              ┌──────────────────────────────────────────┐
              │      FilamentCatalogPickerModal           │
              │  Vendor ▾   Type ▾   [product list]       │
              │  emits:  const EffectiveFilament&         │
              └──────────────────────────────────────────┘
                       │                         │
        on_select (AMS)│                         │ on_select (preset)
                       ▼                         ▼
      AMS edit modal populates its       MaterialSettingsManager persists
      material/brand/temp fields;        {type, filament_id?, brand?, name?,
      existing Save → set_slot_info      temps} (denormalized); preset tap
      → FilamentSlotOverrideStore        heats to stored temps
```

Key structural choice: **the picker is a pure producer.** It loads the catalog, presents
the linked dropdowns, and returns one `EffectiveFilament` by value. The two consumers are
decoupled from it and from each other. This keeps the picker independently testable and
makes the future Phase-3 / spool-wizard reuse trivial.

---

## 4. Component — `FilamentCatalogPickerModal`

**Files:** `include/ui_filament_catalog_picker.h`, `src/ui/ui_filament_catalog_picker.cpp`,
`ui_xml/components/filament_catalog_picker.xml`. Registered in `src/xml_registration.cpp`
(L014 — unregistered components silently fail). Subclass `Modal` (see `MODAL_SYSTEM.md`).

### 4.1 Layout (XML, `ui_dialog`)

```
┌─ Choose Filament ──────────────────────────────┐
│   Vendor  [ Bambu Lab            ▾ ]           │  lv_dropdown  name="vendor_dropdown"
│   Type    [ PLA                  ▾ ]           │  lv_dropdown  name="type_dropdown"
│   ┌──────────────────────────────────────┐    │
│   │ ✓ Bambu PLA Basic        220° / 55°  │ ▲  │  scrollable container name="product_list"
│   │   Bambu PLA Matte        220° / 55°  │    │  (rows built in C++)
│   │   Bambu PLA Silk         230° / 60°  │ ▼  │
│   └──────────────────────────────────────┘    │
│                 [ Cancel ]    [ Select ]       │  modal_button_row
└────────────────────────────────────────────────┘
```

Design tokens only (colors/spacing via `#tokens`, `text_*` widgets). Product rows are built
programmatically; the checkmark on the current selection uses the **icon font**, resolved via
`lv_xml_get_font(nullptr, lv_xml_get_const(nullptr, "icon_font_xs"))` + `ui_icon::lookup_codepoint("check")`
— **not** `LV_SYMBOL_OK`, which renders as tofu on body-font labels (L097). Product name in
`font_body`; temps right-aligned in a fixed-width column so names stay aligned.

### 4.2 Public interface

```cpp
class FilamentCatalogPickerModal : public Modal {
  public:
    using SelectCallback = std::function<void(const helix::printer::EffectiveFilament&)>;
    // seed_type: pre-select the Type dropdown (preset path). std::nullopt = free (AMS path).
    void show(lv_obj_t* parent, std::optional<std::string> seed_type, SelectCallback on_select);
};
```

### 4.3 Lifetime & data flow

- On `show()`, load the catalog **once** into a member (`FilamentCatalog catalog_ = load_full();`).
  It lives for the modal's open duration and frees on dismiss — matches the transient / zero-idle
  design (spec-1 §2, §4.2). Because `EffectiveFilament*` returned by the catalog point into its
  `products_` vector, the modal holds the catalog **by member, never copied** (copy-ctor is
  `=delete`d — see §5).
- **Populate Vendor** = `catalog_.all_brands()`, "Generic" pinned first, then remaining brands
  alphabetical. (Recently-used-brand pinning is optional polish — see §11.)
- **Populate Type** = `catalog_.types_for_brand(current_vendor)`.
- **Populate product list** = `catalog_.products_for(current_vendor, current_type)`, one row each.
- **Linkage:** vendor change → refill Type dropdown + product list; type change → refill product
  list. Rebuild rows with `helix::ui::safe_clean_children(product_list)` (never `lv_obj_clean` in a
  queued/deferred context — L059/L081).
- **Seeding:** if `seed_type` is set, default Vendor to the first brand carrying that type
  (prefer "Generic"), set Type to `seed_type`. If not, default Vendor="Generic", Type=first
  available for Generic.
- **Select:** copy the highlighted `EffectiveFilament` **by value**, dismiss, then invoke
  `on_select` with the copy. Nothing dangles after `catalog_` frees.

### 4.4 Threading

The modal and catalog load run on the main thread (user-initiated open; a flat-catalog parse of
356 records is cheap). No background threads introduced. If a slow device (AD5M/K1) hitches on
open, mitigation is an async load (§10 risk) — not built unless measured.

---

## 5. `FilamentCatalog` API additions

`include/filament_catalog.h` / `src/printer/filament_catalog.cpp`:

```cpp
std::vector<std::string> types_for_brand(const std::string& brand) const;   // NEW
std::vector<const EffectiveFilament*> products_for(const std::string& brand,
                                                   const std::string& type) const;  // NEW
std::vector<std::string> brands_for_type(const std::string& type) const;    // NEW (symmetry/tests)
```

Fold in the Phase-1 deferred minors while here:
- `all_brands()` O(n²) → build via a `std::set` / dedup pass in `index()`.
- **`FilamentCatalog(const FilamentCatalog&) = delete;`** — the picker holds it by member and
  external `EffectiveFilament*` must not be invalidated by an accidental copy.
- Restore `load_full()`'s per-path parse-fallthrough lost in Phase 1 (a malformed overlay path
  should not abort the built-in load).

Build brand→types and (brand,type)→products indices in `index()` so the dropdown queries are
O(1)/O(k), not repeated linear scans.

---

## 6. AMS-slot integration (Phase 2a)

**Entry point:** the AMS edit modal's existing material selector — `material_dropdown`
(`ui_xml/ams_edit_modal.xml:132`), today a plain type-only `lv_dropdown`.

**Change:** replace it with a tappable **"Material: `<type>` ▸"** field that opens
`FilamentCatalogPickerModal` seeded with the slot's current type (modal-over-modal; `ModalStack`
handles the stack, Cancel returns to the AMS edit modal).

**On select:** the picker's `EffectiveFilament` **populates the AMS edit modal's own fields** —
material (=`type`), brand, nozzle/bed temps, and a derived spool name (`brand + " " + name`).
The user may still tweak before saving. Commit happens through the modal's **existing Save
path** (`ui_ams_edit_modal.cpp:874` → `AmsBackend::set_slot_info(slot, SlotInfo)` →
`FilamentSlotOverrideStore`) — no new write plumbing.

**Field mapping** (`EffectiveFilament` → `SlotInfo`, `include/ams_types.h:634`), mirroring the
existing QR-scan block (`ui_ams_edit_modal.cpp:844-883`):

| EffectiveFilament | SlotInfo |
|---|---|
| `type` | `material` |
| `brand` | `brand` |
| `name` | contributes to `spool_name` (`brand + " " + name`) |
| `nozzle_min` / `nozzle_max` | `nozzle_temp_min` / `nozzle_temp_max` |
| `bed_temp` | `bed_temp` |
| (no per-product color in catalog) | `color_rgb` left unchanged / user-set |

Spoolman's "Choose Spool" flow is untouched and orthogonal.

---

## 7. Preset integration + persistence (Phase 2b)

**Entry point:** the preset long-press, today `material_picker_.show(...)`
(`src/ui/ui_panel_filament.cpp:780`, via `handle_preset_longpress` at :770). Replace the
type-only `MaterialPickerMenu` with `FilamentCatalogPickerModal`, seeded with the button's
current type (`preset_materials_[slot]`).

Changing the picker's Type dropdown and choosing a **Generic** product *is* a plain type-swap,
so the new picker fully subsumes today's type-only reassignment. The "Reset all to defaults"
affordance from `MaterialPickerMenu` is preserved (a secondary action in the new modal, or the
existing reset entry retained). `MaterialPickerMenu` becomes unused (only caller is the preset
long-press) — retire the class + `material_picker_menu.xml` in 2b once the picker is wired.

### 7.1 Persistence model

`MaterialSettingsManager` per-slot entry grows from a bare **type string** to:

```jsonc
{ "type": "PLA",            // material type (always present; drives generic fallback)
  "filament_id": "orca_bambu_pla_matte",   // optional: set when a branded product is picked
  "brand": "Bambu Lab",     // optional (display)
  "name": "PLA Matte",      // optional (display)
  "nozzle": 220,            // denormalized at pick time
  "bed": 55 }
```

- **Denormalized temps** are stored at pick time so a preset **tap never loads the catalog**
  (`FilamentCatalog` stays transient — no resident RAM cost, no per-tap parse). Empty
  `filament_id` ⇒ generic behavior: temps come from `filament::find_material(type)` as today.
- API: keep `set_preset_material(slot, type)` (type-only path); add
  `set_preset_filament(slot, const EffectiveFilament&)` (branded path, denormalizes + persists).
- **Button behavior:** tap heats to the stored temps (branded when set, else generic).
- **Label:** type-primary ("PLA"). A branded preset may show brand as secondary text; the temps
  line reflects the stored (branded) temps.

### 7.2 Config migration

Bump the settings schema version and migrate old preset entries (bare `"PLA"` string, or
slot→string map) to `{type: "PLA"}` (no `filament_id`). Follow `CONFIG_MIGRATION.md`. The
migration MUST be lossless and MUST NOT brick existing settings. Guard the JSON reads:
default-constructed `nlohmann::json` is null and `.value()` throws — check `is_object()` /
`is_string()` before access (L087).

---

## 8. Dead-code removal

`ui_xml/filament_preset_edit_modal.xml` is registered (`src/xml_registration.cpp:595`) but never
shown — no `Modal::show`, no `filament_preset_edit_*_cb` definitions. It is a manual
name + nozzle/bed spinbox editor whose function belongs to Phase 3 (user-editable custom
filaments), not branded selection. **Delete the `.xml` + its registration line** in Phase 2. If
Phase 3 wants a manual editor, the form is recoverable from git history.

---

## 9. Testing

| Level | Coverage |
|---|---|
| Unit | New catalog queries (`types_for_brand`, `products_for`, `brands_for_type`); `all_brands` dedup |
| Unit | `EffectiveFilament` → `SlotInfo` field mapping |
| Unit | Preset persistence round-trip **+ old→new config migration** (lossless; malformed/legacy input) |
| Widget (`XMLTestFixture`) | Picker linkage: vendor change refilters Type + product list; seeded type pre-selects; Select emits the right `EffectiveFilament` |
| Widget | Checkmark renders via icon font (guard L097 tofu) |
| Lint | `[filament_data]` (existing) covers catalog coherence |

Async setters in tests must `UpdateQueue::instance().drain_queue_for_testing()` before asserting
(L048). Test-only hooks use the friend-`*TestAccess` pattern, not public `*_for_testing()` methods (L065).

**Process:** implementers run builds in the **foreground** and verify completion via `git log`,
not agent chat — Phase-1 subagents repeatedly stalled backgrounding `make`
(`project_filament_catalog_phase1.md`).

---

## 10. Risks

- **Catalog parse on modal open** (slow devices): a flat 356-record parse on the main thread.
  Measure on AD5M/K1; if it hitches, load async and show a brief spinner. Not built pre-emptively.
- **Config migration**: highest-risk item — a bad migration corrupts user settings. Test legacy,
  empty, and malformed inputs; migration is lossless-or-abort.
- **`MaterialPickerMenu` retirement**: confirm no other caller before deleting (grep says preset
  long-press is the only one). Its "Reset to defaults" affordance must survive.
- **Modal-over-modal** (picker over AMS edit modal): verify Cancel/back returns to the AMS modal
  and the picker's catalog frees on dismiss.
- **Vendor-order vs spec-1 §11**: spec-1 recorded a type-outer ordering; this phase deliberately
  reverses it (§2). Update spec-1 §11 cross-reference, or note the supersession here.

---

## 11. Out of scope (Phase 3+)

- **User editability UI** — edit built-ins + author custom entries via the read-write
  `config/user_filaments.json` overlay (load/merge path exists from Phase 1). Manual preset
  name+temps editing (the deleted `filament_preset_edit_modal.xml` concept) lands here.
- **Recently-used / favorite brand pinning** beyond "Generic" — light polish; include only if cheap.
- **Per-vendor product-count badges** in the dropdown — optional; skip unless it reads well.
- Snapmaker U1 `codes.snapmaker` decode, Bambu tag decode (spec-1 §11, unchanged).

---

## 12. Implementation staging

- **2a — standalone deliverable:** `FilamentCatalog` API additions (§5) → `FilamentCatalogPickerModal`
  (§4) → AMS-slot integration (§6). Ships a working offline picker for slots on its own.
- **2b — depends on 2a:** preset long-press upgrade (§7) + `MaterialSettingsManager` schema growth +
  config migration + `MaterialPickerMenu` retirement + dead-code removal (§8).

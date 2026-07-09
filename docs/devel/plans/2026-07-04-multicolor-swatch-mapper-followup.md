# Multi-color swatches for filament-mapper surfaces

**Date:** 2026-07-04
**Status:** Planned
**Follow-up to:** `feat(spoolman): multi-color diagonal swatches + picker layout polish` (commit `5166eeaa1`), which shipped `helix::ui::apply_swatch_color` and applied it to the Spoolman picker rows, AMS-panel slot chips, and the AmsEditModal form swatch.

## Problem

`helix::ui::apply_swatch_color(swatch, primary_rgb, multi_color_hexes)` renders a filament color swatch as equal-area diagonal chunks when the spool is multi-color (Spoolman `multi_color_hexes`), and as a solid delineated fill otherwise. It reaches every place multi-color **data** is in scope today.

The filament-mapper UIs (tool↔slot assignment, print-select, preflight) do **not** get multi-color, because they read `helix::AvailableSlot` / `helix::GcodeToolInfo` (`include/filament_mapper.h`), an intentional LVGL-free abstraction boundary that carries only `uint32_t color_rgb`. The `SlotInfo → AvailableSlot` conversion drops `multi_color_hexes`, so the data never arrives.

## Goal

Thread `multi_color_hexes` through the `AvailableSlot` boundary and render it on the **large, label-free slot swatches** in the mapping flow. Keep the change surgical: one data-model field, one conversion line, three call-site swaps.

## Design decisions (resolved during brainstorming)

1. **Slot side only.** `GcodeToolInfo` stays single-color — a G-code file assigns exactly one color per tool; multi-color is not a concept there. Only the *slot/actual* side carries `multi_color_hexes`.
2. **Print-select detail chip stays single-color.** `filament_swatch.xml` is a small (40px) labeled two-tone comparison tile; its bottom band already carries a centered lane-number label whose contrast color is computed against a single background. Diagonal chunks under that label make contrast ambiguous. Not worth it — the full treatment shows on the larger swatches.
3. **Mapping-card dots stay single-color (primary).** `gcode_dot` / `slot_dot` in `ui_filament_mapping_card.cpp` are small round dots; diagonal chunks read as busy/illegible at that size. They keep the primary color.
4. **Empty slots keep their warning-border treatment.** Several sites paint a warning border on empty slots. `apply_swatch_color` sets its own muted delineating border, which would clobber that. Rule: route **only non-empty slots** through `apply_swatch_color`; leave the empty-slot branch exactly as-is.

## Implementation

### 1. Data model — carry the field across the boundary
- `include/filament_mapper.h`: add `std::string multi_color_hexes;` to `struct AvailableSlot` (mirrors `SlotInfo`/`SpoolInfo`; a plain `std::string`, so `FilamentMapper` keeps its LVGL-free boundary). `GcodeToolInfo` unchanged.
- `src/printer/ams_state.cpp`, `AmsState::collect_available_slots()` (~line 747, next to `as.color_rgb = slot_info.color_rgb;`): add `as.multi_color_hexes = slot_info.multi_color_hexes;`. This is the single central builder — one line feeds every consumer.

### 2. Call-site swaps (multi-color slot swatches)
Replace the raw `lv_obj_set_style_bg_color(swatch, lv_color_hex(slot.color_rgb), 0)` with
`helix::ui::apply_swatch_color(swatch, slot.color_rgb, slot.multi_color_hexes)` (add `#include "ui_swatch.h"`), **guarded to non-empty slots**:

1. `src/ui/ui_filament_slot_picker.cpp` (~:155) — the slot-row `swatch`. Empty-slot branch (~:152, warning border) left untouched.
2. `src/ui/ui_filament_mapping_modal.cpp` (~:157) — the `chosen_swatch` (slot side). `expected_swatch` (gcode, ~:130) left single-color. Empty branch (~:162) untouched.
3. `src/ui/modals/ui_preflight_check_modal.cpp` (~:162) — the `seated_swatch`. `intended_swatch` (~:152) left single-color.

### 3. Explicitly unchanged
- `ui_filament_mapping_card.cpp` — `gcode_dot` / `slot_dot` stay primary (decision 3).
- `ui_print_select_detail_view.cpp` — two-tone chip stays primary (decision 2).
- Gcode-side / spool wizard — out of scope.

## Testing

- **Unit (new plumbing):** add/extend an `ams_state` test asserting `multi_color_hexes` survives `collect_available_slots()` for a slot whose backing `SlotInfo` carries it. This is the load-bearing change.
- **Helper:** `apply_swatch_color` is already unit-tested (`tests/unit/test_ui_swatch.cpp`).
- **Visual (mock, 800×480):** load a multi-color mock spool into a slot; confirm diagonal chunks in (a) the filament slot picker, (b) the mapping modal `chosen_swatch`, (c) the preflight `seated_swatch`. Confirm empty slots still show the warning border, and that mapping-card dots + the print-select chip stay solid primary.

## Risks / notes

- The three target swatches are label-free squares, so no label-contrast issue (unlike the print-select chip).
- Watch the empty-slot guard at each site — the single easy mistake here is clobbering the warning border by routing empty slots through `apply_swatch_color`.
- No threading/lifecycle surface: `AvailableSlot` is a value struct; the render calls are on the main thread in existing paint paths.

## Out of scope

Gcode-side (expected/intended) swatches, the spool wizard, the print-select detail chip, and the mapping-card dots.

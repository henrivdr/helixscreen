# Pre-flight Filament Validation & Remap — Design Spec

**Date:** 2026-06-16
**Status:** Design approved, pending spec review → implementation plan
**Author:** Preston (with Claude)

## Problem

A user printing a 2-color file on a Snapmaker U1 loaded filament into physical heads
T0 and T2 (slots 1 and 3) and sliced accordingly, but the print auto-fed head **T1**
(unloaded) and cancelled on filament runout ~20 minutes in. The user also reports there
is no way to remap color→head on screen (a capability the stock UI had). The failure
reproduces from the slicer and web UI too — so the *mapping* fault is below HelixScreen,
but the **UX failure is ours**: we had every fact needed to catch this before the print
started, and surfaced nothing until the runout.

This is not greenfield. A pre-flight gate already partially exists and is broken in three
specific ways (below). The work is **harden + correct + enrich**, plus a remap path.

### Root causes (verified in code + on a live U1)

1. **Synthetic tool-index bug (cosmetic + functional).**
   `ui_print_select_detail_view.cpp:351-354` seeds tool indices from *palette position*
   (`{0..filament_colors.size()-1}`), not actual gcode tools. A T0+T2 file renders as
   "T0/T1" and the empty-tool check inspects the wrong heads. The correct set
   (`tools_used_indices = {0,2}`) only arrives after the gcode viewer parses, via the
   re-call at `ui_print_select_detail_view.cpp:829`.

2. **Parse race.** The Print button is enabled by `print_select_can_print`
   (`ui_panel_print_select.cpp:307`), which is *not* gated on parse completion. If the
   user taps Print before parse, `has_empty_tool_warning()` is `false` (no parsed tools
   yet) and the existing gate at `ui_panel_print_select.cpp:2515` waves it through.

3. **Detection coupled to remap-card visibility.** Color/material mismatch detection
   exists and works (`FilamentMapper`), but it only runs when `FilamentMappingCard`
   would show, and the card hides for backends with no editable mapping
   (`ui_filament_mapping_card.cpp:67`) — i.e. **exactly Snapmaker U1 and ACE**. So the
   printers in this report get *no* mismatch detection at all.

## Goals / Non-goals

**Goals**
- Block a print before it starts when a required head is empty, with a clear, actionable
  explanation (which head, which color).
- Make the FILAMENTS display honest: real tool indices, intended→seated color/material.
- Run mismatch detection for **all** AMS backends, not just editable ones.
- Give back a remap capability, including for U1/ACE (no native remap API).

**Non-goals**
- Fixing the upstream slicer tool-assignment compaction (out of our control).
- Color match as a hard gate (kept display-only — fuzzy on glow/translucent).
- Multi-unit / cross-backend remap topologies beyond what each backend already supports.

## Decisions (locked)

| Topic | Decision |
|-------|----------|
| Scope | All three layers: gate hardening (L1), honest display (L2), remap (L3). |
| Enforcement | **Empty required head → hard-block** (only when *definitively* empty). **Material mismatch → advisory** (allow Print Anyway). **Color mismatch → display-only.** |
| Detection | Reuse `FilamentMapper` (`colors_match`/`materials_match`/`color_distance`, tolerance 50). Decouple from card visibility. |
| Remap — native backends | AFC, Happy Hare, CFS, AD5X-IFS, toolchanger: reuse `FilamentMappingModal` → existing `set_tool_mapping`. |
| Remap — U1 / ACE | **Comprehensive gcode rewrite** (no native API; no map_table in file). Validate on bench U1 (192.168.30.103). Behind HelixPrint-plugin history guard. |
| History safety | All gcode-rewrite remap goes through `GCodeFileModifier` + HelixPrint plugin (symlink + `history.modify_job`), same guard the bed-leveling-disable feature uses. |

## Architecture

One new pure-logic module; everything else is reuse + decoupling.

```
parsed gcode (tools_used_indices) ─┐
intended colors/materials ─────────┼─► PreflightValidator ─► Vec<ToolCheck> ─┬─► L1 gate (block/warn)
AMS backend live slot state ───────┘     (reuses FilamentMapper)             ├─► L2 display (swatches+icon)
                                                                              └─► L3 remap entry (if strategy≠None)
```

### `PreflightValidator` (new — `src/printer/preflight_validator.{h,cpp}`)

Pure, no LVGL. Backend-agnostic. Runs whenever AMS is present and the file declares
tools — **independent of `FilamentMappingCard::should_show()`**.

```cpp
struct ToolCheck {
    int tool_index;              // real gcode tool (from tools_used_indices)
    uint32_t intended_color;     // slicer palette
    std::string intended_material;
    int mapped_slot;             // resolved physical slot/head for this tool
    bool slot_present;           // definitively seated? (false ⇒ block candidate)
    bool color_ok;               // FilamentMapper::colors_match
    bool material_ok;            // FilamentMapper::materials_match
    enum class Severity { Ok, ColorMismatch, MaterialMismatch, EmptySlot } severity;
};

struct PreflightResult {
    std::vector<ToolCheck> checks;
    bool has_block() const;      // any EmptySlot
    bool has_advisory() const;   // any MaterialMismatch (color is display-only)
};
```

Inputs: `tools_used_indices` (parsed gcode), per-tool intended color/material
(palette/metadata), and **live** AMS slot state. Reuses `FilamentMapper`. Consumed by
the gate, the display, and the remap entry point.

**U1 live-state source (preferred over inference):** the Snapmaker backend should read
`print_task_config.filament_exist[]` / `filament_color_rgba[]` / `filament_type[]` —
verified live, these are per-physical-head seated/color/material truth. Wire this into
`ams_backend_snapmaker.cpp`'s `get_slot_info()` if not already authoritative.

## L1 — Pre-flight gate (`ui_panel_print_select.cpp::start_print`)

Insert before the existing empty-tool check at `ui_panel_print_select.cpp:2515`.

- **Close the parse race:** the gate evaluates only once `gcode_loaded_ == true`. If the
  user taps Print before parse completes, show a brief "Checking filaments…" and resolve
  when the parse lands. Never evaluate against synthetic tools.
- **Enforcement:** `PreflightResult::has_block()` (empty head) → hard-block.
  `has_advisory()` (material) → warn but allow Print Anyway. Color → not in gate.
- **Definitively-empty only:** block on `get_slot_info(slot).has_filament_info() == false`
  (or U1 `filament_exist[slot] == false`). Ambiguous/unknown seated reads degrade to
  advisory, never a false block.
- **Enriched modal** (replaces bare "Print Anyway / Cancel"):

```
 ┌──────────────────────────────────────────────┐
 │  ⚠  Filament check — 1 problem                 │
 │                                                │
 │   T0  ● Magenta   → Head 1  ● Magenta    ✓     │
 │   T2  ● Green     → Head 3  ○ EMPTY      ✗     │
 │                                                │
 │  Head 3 is empty. This print will run out      │
 │  on the second color.                          │
 │                                                │
 │   [ Remap… ]      [ Cancel ]   [ Print Anyway ]│
 └──────────────────────────────────────────────┘
```

`[Remap…]` shown only when the backend's `RemapStrategy != None`. Modal: either extend
`modal_show_confirmation` (`ui_modal.h:436`) usage or a small `Modal` subclass with a
tertiary button (`wire_tertiary_button`, `ui_modal.h:262`).

## L2 — Honest display (`ui_print_select_detail_view.cpp`)

- **Kill the synthetic-index seed:** remove the `{0..palette.size()-1}` construction at
  `:351-354`. Compute swatches/labels strictly from `tools_used_indices`. Until parse
  completes, show a neutral "analyzing" state rather than a guessed set.
- **Decouple detection:** drive the `filament_mismatch` subject + warning icon
  (`print_file_detail.xml:203`) from `PreflightValidator` for **all** AMS backends, not
  gated by `FilamentMappingCard::should_show()`. U1/ACE finally get the warning.
- Swatch shows intended→seated per tool, with ✓ / ⚠ per `ToolCheck::severity`.

## L3 — Remap (`RemapStrategy` per backend)

Add a capability to the backend interface:

```cpp
enum class RemapStrategy { None, Native, GcodeRewrite };
virtual RemapStrategy get_remap_strategy() const { return RemapStrategy::None; }
```

- **`Native`** — AFC (`SET_MAP`), Happy Hare (`MMU_TTG_MAP`), CFS (`BOX_MODIFY_TN`),
  AD5X-IFS (`_IFS_VARS`), toolchanger (`ASSIGN_TOOL`). Reuse `FilamentMappingModal`
  (`ui_filament_mapping_modal.h`) → existing `set_tool_mapping`. Just make it reachable
  from the gate's `[Remap…]`. No new mapping logic.

- **`GcodeRewrite`** — U1, ACE. Same modal UI (logical tool → physical head picker), but
  applied by rewriting the gcode via `GCodeFileModifier` (`gcode_file_modifier.h`) and
  printing through the HelixPrint plugin (`moonraker-plugin/helix_print.py`,
  `start_modified_print`) so history/stats stay under the original filename.

  **U1 rewrite is comprehensive** — verified against a real OrcaSlicer 4-color file on
  the bench U1. There is **no `extruder_map_table` in the file**; mapping is
  identity-baked (`Tn`, n = physical head). A remap of logical tool *a*→head *b* must
  rewrite **all three** command families consistently:
  1. Prestart block: `SM_PRINT_AUTO_FEED EXTRUDER=a` / `SM_PRINT_EXTRUDER_PREHEAT
     EXTRUDER=a` / `SM_PRINT_FLOW_CALIBRATE EXTRUDER=a` → `=b`. *(The `AUTO_FEED EXTRUDER=<empty head>`
     line is the actual runout-cancel trigger.)*
  2. Body: every bare `Ta` toolchange → `Tb`.
  3. Temps: `M104/M109 … Ta` tool-suffixed → `Tb`.

  Missing any one family yields wrong temps or a stale feed. The rewrite must be driven
  by an exhaustive scan, not a single pattern.

  **ACE** uses `ACE_CHANGE_TOOL TOOL=n` — simpler single-family rewrite; confirm against
  a real ACE file before shipping (no ACE hardware here).

### History safety (already solved — reuse)

`ui_print_preparation_manager.cpp` already does download → modify → upload-temp →
print-via-plugin. With the HelixPrint plugin, the job's history entry is patched back to
the original filename + per-file stats (`helix_print.py:625-634`). Without the plugin,
the existing flow refuses modifications rather than pollute — L3 inherits that guard via
`check_helix_plugin()` (`moonraker_job_api.cpp:81-107`).

## Risks & validation

- **U1 rewrite unvalidated until bench test.** We have a U1 at 192.168.30.103. Validate
  the full rewrite end-to-end (2-color file → remap → print pulls the correct heads, no
  runout, temps correct) before enabling for users. Gate behind a flag until confirmed.
- **Seated-signal reliability.** Hard-block only on definitively-empty so a flaky read
  degrades to advisory, not a false block that traps the user.
- **Exhaustive rewrite scan.** Unit-test the U1 rewrite against the captured 4-color
  fixture: assert every `Tn`, `SM_PRINT_*_EXTRUDER=`, and `M10x …T` site is remapped and
  no other lines change.
- **Color matching fuzz.** Glow/translucent filaments confuse color distance — that is
  precisely why color is display-only, never a block.

## Key file references

| Concern | Location |
|---------|----------|
| Synthetic-index bug | `ui_print_select_detail_view.cpp:351-354` |
| Correct tools available | `ui_print_select_detail_view.cpp:801-833` (`tools_used_indices`) |
| Empty-tool publish | `ui_print_select_detail_view.cpp:692`; `has_empty_tool_warning()` `ui_print_select_detail_view.h:191` |
| Existing gate | `ui_panel_print_select.cpp:2503-2543` (insert at `:2515`) |
| Print button enable | `ui_panel_print_select.cpp:307` (`print_select_can_print`) |
| Mismatch detection (reuse) | `FilamentMapper` `src/printer/filament_mapper.cpp:52-119`; structs `filament_mapper.h:15-53` |
| Card visibility gate (the coupling) | `ui_filament_mapping_card.cpp:67-86` |
| Mismatch icon | `print_file_detail.xml:203` (`filament_mismatch` subject) |
| Remap modal (reuse) | `ui_filament_mapping_modal.h:23-82` |
| Native remap emitters | AFC `ams_backend_afc.cpp:2577`; HH `ams_backend_happy_hare.cpp:2154`; CFS `ams_backend_cfs.cpp:1218`; AD5X `ams_backend_ad5x_ifs.cpp:1612`; toolchanger `ams_backend_toolchanger.cpp:500` |
| U1 not_supported | `ams_backend_snapmaker.cpp:672` |
| Gcode modifier (reuse) | `gcode_file_modifier.h`; flow `ui_print_preparation_manager.cpp:1072-1336` |
| HelixPrint plugin | `moonraker-plugin/helix_print.py` (`start_modified_print`, history patch `:625-634`) |
| Plugin detection | `moonraker_job_api.cpp:81-107` (`check_helix_plugin`) |
| U1 live mapping facts | memory `reference_u1_tool_mapping_mechanism.md`; `print_task_config.{extruder_map_table,filament_exist,filament_color_rgba}` |

## Testing

- Unit: `PreflightValidator` over fixtures — empty head, material mismatch, color
  mismatch, all-ok, parse-not-ready. Verify severity + block/advisory partitioning.
- Unit: U1 gcode rewrite against the captured `4_COLOR_RING` fixture — exhaustive
  three-family remap, no collateral edits.
- Integration: gate blocks before the print RPC fires when a head is empty; advisory
  allows Print Anyway; gate waits for parse.
- On-device (bench U1): full remap round-trip, history entry shows original filename.

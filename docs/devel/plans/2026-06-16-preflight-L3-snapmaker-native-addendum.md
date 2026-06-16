# L3 Addendum — Snapmaker U1 Remap via Native `print_task_config` Commands

**Date:** 2026-06-16
**Status:** Spec addendum, pending review → rebuild L3 for U1
**Supersedes:** the U1 `GcodeRewrite` apply path in `2026-06-16-preflight-filament-validation.md` (Tasks 11–12). L1 (gate), L2 (honest display), and the enriched modal UX are UNCHANGED.

## Why this addendum exists

Earlier research wrongly concluded the U1 had no native remap mechanism, so L3 was built as a gcode rewrite (`GcodeToolRemapper`) printed through the HelixPrint plugin. That was wrong. The U1's `print_task_config` Klipper extra (`klippy/extras/print_task_config.py`) exposes a native command API — the exact one the stock screen used. Full reference: `docs/devel/SNAPMAKER_U1_PRINT_TASK_CONFIG.md`. We pivot the U1 apply path to that API: simpler, firmware-blessed, matches stock UX, no temp files / no history patching / no plugin dependency.

## The native commands (sent BEFORE `PRINT_START` — they error mid-print, id 531)

- `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv physical 0..3>` — declares which physical heads the task uses. The prestart macros (`sm_print_auto_feed`, `sm_print_extruder_preheat`, `sm_print_check_switch_extruder`) all gate on `extruders_used[n]`, so heads NOT listed are never fed/preheated even though the slicer baked `SM_PRINT_AUTO_FEED EXTRUDER=n` unconditionally.
- `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<0..31 logical> MAP_EXTRUDER=<0..3 physical>` — remaps a logical tool to a physical head (`extruder_map_table[config]=phys`). Drives both `T0`–`T3` and the `T4`–`T31` extended macros.
- `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=<0..3> FILAMENT_COLOR_RGBA=… FILAMENT_TYPE=…` — per-head filament metadata (optional for us; the device already has live `filament_exist/color_rgba/type` from feed/RFID).

Each new print resets `extruder_map_table` to identity and `extruders_used` to all-False, so HelixScreen must (re)send these every print.

**Index discipline:** `CONFIG_EXTRUDER` in `SET_PRINT_EXTRUDER_MAP` is logical (0–31); `MAP_EXTRUDER`, `SET_PRINT_USED_EXTRUDERS`, and `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER` are physical (0–3).

## Design

### Part A — Always-on spurious-feed fix (no user action; fixes the reporter's bug)

The reporter's runout is NOT a tool-collapse — it's the prestart feeding heads the print never uses. Real file `lid_PLA_6m28s.gcode`: body uses heads 0 and 2 (`T0`,`T2`), but the baked prestart auto-feeds 0,1,2,3. Unload head 1 → `SM_PRINT_AUTO_FEED EXTRUDER=1` hits an empty lane → runout-cancel.

**Fix:** before every U1 `print_start`, HelixScreen sends `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<physical heads the body actually uses>`. For the reporter's file that's `0,2` → heads 1 and 3 feed/preheat become no-ops → no runout. This requires **zero** user interaction and fixes the case transparently.

- "Heads the body uses" = `{ extruder_map_table[t] for t in tools_used_indices }` (post-remap physical set; identity map until the user remaps). Source `tools_used_indices` from the parsed gcode (we already have it).
- Send via the print-start path (repurpose `PrintStartController::apply_remap()` — it already runs pre-`print_start`).

### Part B — Validator required-heads (so the gate is correct)

The gate must block when a head the print needs is empty. With the native model:
- For each body tool `t`: physical head `h = extruder_map_table[t]` (identity unless remapped). Required physical heads = `{ h }`.
- Empty check against live `print_task_config.filament_exist[h]` (already the truth `AmsState::collect_available_slots()` surfaces).
- Block if any required head is empty. (Spurious prestart-only heads are NOT required — Part A suppresses them — so we do NOT block on them.)

This replaces the current "required = body `Tn`, checked via mapper default" with "required = post-map physical heads, checked via `filament_exist`." Net: the gate blocks exactly the heads that, after our `SET_PRINT_USED_EXTRUDERS`, the firmware will actually feed and find empty.

### Part C — Remap (the relocate case) via native commands

When the user loaded a color into a different physical head than the file's logical tool expects, Remap sends, before `print_start`:
1. `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<logical tool> MAP_EXTRUDER=<chosen physical head>` per remapped tool.
2. `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<post-remap physical set>` (Part A, recomputed from the new map).
3. (optional) `SET_PRINT_FILAMENT_CONFIG` if we want to push color/material.

The enriched modal (Task 8) and `recompute_preflight()` (Task 10) are reused unchanged — only the *apply* swaps from `set_tool_mapping`/gcode-rewrite to emitting these gcode commands. The modal's slot picker yields logical-tool → physical-head, which maps 1:1 to `SET_PRINT_EXTRUDER_MAP`.

### Part D — Strategy + scope changes

- Replace U1's `RemapStrategy::GcodeRewrite` with a new `RemapStrategy::SnapmakerNative` (or fold into `Native` with a Snapmaker-specific apply). ACE stays `None` (unchanged).
- **KEEP `GcodeToolRemapper` + `modify_and_print_with_remap` — do NOT delete.** They are a distinct, tested, generic capability (rewrite slicer-baked tool commands + print via the history-preserving HelixPrint plugin), not redundant code. `RemapStrategy::GcodeRewrite` remains the **generic fallback for backends with no native remap API**. U1 simply moves *off* it to `SnapmakerNative`; it currently has no active consumer, which is acceptable for kept infrastructure with a foreseeable user.
  - Reusability boundary: the *wiring* (`modify_and_print_with_remap`, GCodeFileModifier, plugin) is backend-agnostic. The *command families* in `GcodeToolRemapper` (`Tn`, `SM_PRINT_*_EXTRUDER=`, `M10x …T`) are U1-shaped. To serve another backend the remapper must become **family-parameterized per backend** (the backend supplies its tool-command patterns). 
  - **First real consumer: ACE** (`ACE_CHANGE_TOOL TOOL=n`). When we add ACE's family + an ACE fixture, ACE flips from `None` back to `GcodeRewrite`. That generalization is the trigger to parameterize the families (vs hardcoding U1's). Until then, leave the families as-is and documented as U1-derived.
- The HelixPrint plugin remains for the bed-leveling-disable feature regardless.

## What stays vs changes

| Layer | Status |
|-------|--------|
| L1 gate (parse-wait, block, modal entry) | KEEP |
| L2 honest display / decoupled detection / `AmsState::collect_available_slots()` | KEEP |
| Enriched `PreflightCheckModal` (Task 8) | KEEP |
| Native-backend remap (AFC/CFS/HH/…) via FilamentMappingModal + `set_tool_mapping` (Task 10) | KEEP |
| Validator required-heads input | CHANGE (Part B) |
| Always-on `SET_PRINT_USED_EXTRUDERS` pre-print for U1 | NEW (Part A) |
| U1 remap apply | CHANGE: native commands (Part C), not gcode-rewrite |
| `GcodeToolRemapper` / `modify_and_print_with_remap` | KEEP as generic `GcodeRewrite` fallback (U1 stops using it; ACE is the first future consumer once its command family is added) |

## Validation (T13, on bench U1) — the load-bearing checks

1. **Part A alone:** with body-uses-0+2 file and head 1 empty, HelixScreen sends `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` then prints → confirm head 1 is NOT fed and the print completes (no runout). This is the core proof the research flagged as static-only-verified.
2. **Timing:** confirm the command lands in `print_task_config` before the baked prestart block runs (the commands error mid-print, implying they must precede it, but verify on a real print).
3. **Remap:** map a logical tool to a different loaded head via `SET_PRINT_EXTRUDER_MAP`, confirm the body pulls that head and `extruders_used` reflects it.
4. Block fires when a required (post-map) head is genuinely empty.

## Risks

- `SET_PRINT_FLOW_CALIBRATE` is baked by the slicer but is not a defined macro on the current device config — confirm it's a no-op / not load-bearing.
- Each print resets the map/used — must re-send every time; ensure the send happens on reprints and queued prints too.
- Sending these on a non-Snapmaker or firmware-variant device must be gated by backend type (only when the `SET_PRINT_*` commands exist). Probe via `GET_PRINT_TASK_CONFIG` capability or backend identity.
- Bench validation is still required before enabling for users (unchanged posture from the original T13).

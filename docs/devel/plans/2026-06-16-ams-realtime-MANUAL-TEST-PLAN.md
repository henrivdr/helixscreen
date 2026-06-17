# MANUAL TEST PLAN — AMS Real-Time Filament State (Snapmaker U1)

**Device:** U1 @ 192.168.30.103 · **Branch:** `feature/ams-realtime-filament-state`
**Status:** ⏳ NOT YET DEPLOYED — Phase 1 (abstraction) building; Phase 2 (panel binding) next.
**Last deployed build:** _(none for this branch yet — current device build is the preflight branch)_

> I'll bump the Status line + "Enabled this build" column each time I deploy. Run the ⬜ items and
> tell me PASS/FAIL + what you saw. For real-time items, push/pull filament slowly so we can watch.

---

## How to run
1. I deploy + confirm the new build is live (I'll post the version + "go").
2. Open **MULTI-FILAMENT** panel on the U1.
3. Work through the ENABLED tests below. For each: do the action, watch the screen, report result.
4. I'll be tailing `/userdata/helixscreen/helixscreen.log` at debug to confirm the listener→subject
   chain fired (so we can tell "sensor didn't fire" from "UI didn't react").

---

## Test cases

### T1 — Idle runout false-alarm (Phase B — MERGED, ships in first build)
- **Setup:** printer idle (standby), a lane loaded.
- **Action:** manually **Unload** that lane from the panel.
- **✅ Expect:** lane unloads, **NO "filament runout" dialog** pops up.
- **Also:** a genuinely empty idle printer (no load/unload happening) should STILL show the
  load-filament/runout guidance — so we don't want it gone entirely, just suppressed during an
  unload/load op.
- Result: ⬜

### T2 — Real-time filament path (Phase 2)
- **Action:** for each lane, slowly **push** filament toward the toolhead, then **pull** it back.
- **✅ Expect:** the on-screen path line **extends to the toolhead in real time as you push, and
  retracts as you pull** — within a second of the sensor changing (you'll get the "filament
  inserted/removed" toast at the same moment). No more static "always drawn to toolhead."
- Result: ⬜

### T3 — Active-lane indicator is single-sourced (Phase 2 + 3b)
- **Action:** unload the currently-active lane.
- **✅ Expect:** the **bottom T-badge highlight clears** and matches the top-right ("Currently
  Loaded: --- / Idle"). Nothing shows active when nothing is loaded. (Before: top-right said Idle
  but the bottom badge stayed stuck on the unloaded lane.)
- Result: ⬜

### T4 — Operation-card color matches the lane (Phase 3a)
- **Action:** start a **Load** or **Unload** on a lane (e.g. T1 = white) and watch the operation
  card on the right.
- **✅ Expect:** the card's filament **color/name matches the lane you picked** (white for T1) — not
  the next lane's color. ("Current: Slot N" text and the color swatch agree.)
- Result: ⬜

### T5 — Load/Unload menu matches real state (Phase 2)
- **Action:** tap a lane that is **empty/not loaded** → open its menu. Then tap a lane that **is**
  loaded at the toolhead → open its menu.
- **✅ Expect:** empty lane offers **Load** (Unload disabled); loaded lane offers **Unload** (Load
  disabled). No "Unload" offered for a lane that isn't actually loaded.
- Result: ⬜

### T6 — Buffer vs toolhead distinction (Phase 3c, stretch)
- **Action:** unload a lane so filament retracts to the buffer but stays staged (not fully ejected).
- **✅ Expect:** the path shows filament **staged in the buffer** visually distinct from "at the
  toolhead" (different segment/length), rather than looking identical to loaded.
- Result: ⬜

---

## Cross-backend note
These are driven by a backend-agnostic abstraction, so AFC/CFS/Happy Hare/IFS/toolchanger get the
same real-time behavior where their sensors provide the signal (and degrade gracefully where they
don't). U1 is the test vehicle since that's the hardware on hand.

## Parked (separate branch) — preflight Part A bench test
Still owed on `feature/preflight-filament-validation`: with `lid_PLA_6m28s.gcode` (body uses heads
0+2) and head 1 empty, start from HelixScreen → confirm `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2`
fires and head 1 is NOT fed (no runout). Do after the AMS work, or I can deploy that branch first if
you want to knock it out.

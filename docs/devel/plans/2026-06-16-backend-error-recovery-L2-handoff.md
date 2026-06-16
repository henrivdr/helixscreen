# Backend Error-Recovery Overhaul — L2+ Handoff

**Date:** 2026-06-16 · **Branch:** `feature/backend-error-recovery` (worktree `.worktrees/backend-error-recovery`)
**Read first:** `2026-06-16-backend-error-recovery-source.md` (master spec + 12-callout registry), `-L1-spec.md`, `-L1-plan.md`, memory `project_afc_error_recovery_overhaul.md`.

---

## Status of the overhaul

| Layer | Scope | Callouts | Status |
|-------|-------|----------|--------|
| **L0** | Generic `ErrorCenter` core: `ErrorEvent`/`RecoveryAction` model, pure `error_classify::classify` (severity), `PrinterState::is_paused()`, `AmsBackend::classify_error()` hook, `decide_presentation` severity routing. Uncoded pausing `!!` → full-text CRITICAL modal. | E1, E2, E4 | ✅ DONE + Voron-verified |
| **L1** | AFC `classify_error()` (jam text + paused/`error_state_` catch-all) → context-aware recovery actions (Resume/Unload/Eject/Recover, hide-inapplicable); generic multi-button presenter via `ActionPromptModal` (retired `find_recovery`; key840 folds in); error-severity icon on the recovery modal; visibility-gated dedup; integration test of the full glue. E3 audit (overlay sensor render is **live**, no fix). | R1, R3, E3 | ✅ DONE (device-verify Task 7 pending — see below) |
| **L2** | Toolchange step/phase narration — replace the hardcoded step list with real backend narration. | **S1, S2** | ⬜ NEXT |
| **L3** | Polish sweep (independent, can interleave). | **R2, P1, P2** | ⬜ TODO |

**Callout registry (acceptance):** E1✅ E2✅ E4✅ (L0) · R1✅ R3✅ E3✅ (L1) · S1⬜ S2⬜ (L2) · R2⬜ P1⬜ P2⬜ (L3).

### L1 commits (on this branch, after L0's `7f24278ae`)
`d1de8dffd` style field · `92d55edee` AFC classify_error · `fd421e153` build_recovery_prompt · `8b21e3538` multi-button presenter + retire find_recovery · `eff322a7e` error-severity styling + visibility dedup · `874337f23`+`ef4a41722` integration test · `91e199a14` E3 audit doc · `c7c19381e` i18n nits. Suite green (96 shards). Final whole-impl review: **ready to merge**.

### ⚠️ L1 Task 7 (Voron device verification) — STILL PENDING
The automated integration test (`test_gcode_error_routing_e2e.cpp`, `[ui_integration]`) now covers the `process_line → classify_error → present_recovery_modal → ActionPromptModal::show_prompt` glue (asserts modal visible, title "Toolhead jam", `icon_error` shown, Resume button). Device verify is now **confirmatory + screenshot**, not sole coverage. To run it: deploy the pi build to the Voron (192.168.1.112, biqu@; back up `~/helixscreen/bin/helix-screen` first), then with a print paused inject
`RESPOND TYPE=error MSG="Toolhead runout detected by tool_end sensor, but upstream sensors still detect filament. Possible filament break or jam at the toolhead. Please clear the jam and reload filament manually, then resume the print."`
Expect a **red** CRITICAL modal titled "Toolhead jam" offering **Resume / Unload / Recover** (Unload because toolhead loaded), not a dead-end OK. Screenshot for the PR.

### ⚠️ Merge to main is BLOCKED (as of 2026-06-16)
Main's working tree has **uncommitted temperature-consolidation WIP** (touches `src/printer/ams_backend_afc.cpp` among others) + an untracked, **older** copy of `…-source.md` that differs from the branch's committed version. The L1 merge can't proceed until main is clean (Preston to commit/set aside the temp work + drop the superseded untracked `source.md`). Do NOT stash/reset his WIP. Once main is clean: `git checkout main && git merge feature/backend-error-recovery`, verify suite, then worktree cleanup.

---

## L2 — Toolchange step/phase narration (NEXT)

**Problem (grounded, 2026-06-15 Voron):** the toolchange step list is **hardcoded** in `src/ui/ui_ams_sidebar.cpp` (~line 475: Heat→Feed→Purge), driven by the `AmsAction` enum. It ignores AFC's real `//` narration (`AFC_Brush: Clean Nozzle`, `Move to Brush`, `lane N is now loaded in toolhead`, cut/poop/kick/brush/clean/purge), so it **mislabels purge as "Feed" (S1)** and **omits brush/clean/cut/poop/kick steps (S2)**.

**Design direction (from spec §4.3 — confirm/iterate in brainstorm):**
1. A **narration parser** consumes the `//` lines from the gcode-response stream. NOTE: L0's router consumes `!!` (errors) but there is **no `//` narration consumer yet** — L2 likely adds a `GcodeResponseIngestor`-style `//` branch (alongside the existing `notify_gcode_response` hook in `application.cpp:3008` / `GcodeErrorRouter`), tagging narration lines and routing them to a step-model — **NOT** surfacing them as errors. Decide whether this lives in `GcodeErrorRouter`, a new ingestor, or `ActionPromptManager`'s sibling.
2. A **generic step-model interface on `AmsBackend`** maps narration → ordered phases; **AFC provides the first map** (mirrors how L1 made AFC the first `classify_error` adapter). Other backends inherit a no-op default.
3. Replace the hardcoded `ui_ams_sidebar.cpp` step list with steps derived from the backend's step-model. Fixes S1 + S2.

**Open questions for the L2 brainstorm:**
- Where does the `//` ingestor live, and does it share the `notify_gcode_response` subscription with the error router?
- Step-model shape: a fixed phase enum vs. a dynamic ordered list driven by narration (AFC's steps vary: cut/poop/kick/brush/clean aren't all present every swap).
- How does the sidebar observe step changes — a new subject (`toolchange_step`/`toolchange_phase`) updated by the step-model, observed declaratively?
- Backend roster: AFC first; CFS/IFS/etc. get the default no-op until they have a map.

**Verify on Voron:** run a 2-color print, watch a T-swap; the sidebar should show the real sequence (…→ Cut → Purge → Brush → Clean → loaded) instead of a static Heat→Feed→Purge, with no "Feed" shown while purging.

---

## L3 — Polish sweep (independent; can interleave / warm-ups)

- **R2** — AFC RESET lane-picker modal wraps the 4th lane button to a 2nd line (`img5`). Make it responsive (width/wrap) so 4 lanes fit. Find the lane-picker modal (likely an AFC reset/recover overlay).
- **P1** — print preview render goes **blank after resume** (`img7`); related to `project_3d_mode_blank_mid_print` / `project_preview_state_unification`. Re-establish the render on resume/toolchange.
- **P2** — stale lane→hub **tube** stays drawn after an idle-lane unload (`first report`). Traced: the per-slot path canvas only repaints on system `path_filament_segment`/`path_topology` subject changes, **not** on per-lane status — `ui_panel_ams.cpp` `slots_version_observer_` → `refresh_slots()` repaints spools but not the path segment. Route per-slot segment refresh through the same signal. (E3 audit confirmed the sensor *indicators* are live; P2 is specifically the tube-to-hub segment staleness.)

Each is small and isolated; pick up opportunistically.

---

## Process (same as L0/L1)

Use **brainstorming → writing-plans → subagent-driven-development**. Each layer is its own spec + plan + build. Per task: fresh implementer subagent → spec-compliance review → code-quality review → fix loop. TDD, real tests (fail if feature removed), frequent commits. Threading rules from `CLAUDE.md` (AsyncLifetimeGuard, `ui_queue_update`/`tok.defer`, no sync widget deletes in queued callbacks, dynamic-subject lifetime). Test-only access via `*TestAccess` friend classes, never `_for_testing` methods on production classes ([L088]/[L065]). For UI-integration tests, tag `[ui_integration]` (NOT `[.ui_integration]` — the leading dot hides it from `make test-run`/CI).

**Continue with:** start L2's brainstorm (narration ingestor placement + step-model shape are the two real forks), or knock out an L3 warm-up (R2 is the cheapest) while the L1 merge/device-verify settles.

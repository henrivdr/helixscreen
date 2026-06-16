# Backend-Agnostic Error Surfacing & Recovery — Design Spec

**Status:** Draft for review · **Date:** 2026-06-16 · **Author:** brainstormed w/ Preston
**Origin:** Voron / Box Turtle AFC 2-color print failure (192.168.1.112), 2026-06-15.
**Scope:** ALL filament backends + non-AMS Klipper error sources. AFC is the first adapter, not the whole feature.

---

## 1. Motivation

A 2-color print failed during T2/T3 swaps with a toolhead jam. The user flew blind:

- The real fault — *"Toolhead runout detected by tool_end sensor, but upstream sensors still detect filament. Possible filament break or jam at the toolhead. Please clear the jam and reload filament manually, then resume."* — **never reached a dialog.** The print-status screen showed a generic "Filament Runout"; the AMS panels showed bare **"Error"** / **"Error state"**; where the message did appear it was **truncated mid-word** ("upstream senso").
- Recovery was a dead end: **"Eject failed: Unload from toolhead first"** — but no Unload action was offered, even though that is exactly the next step.
- The toolchange **step list was wrong** (showed "Feed filament" while purging; skipped brush/clean entirely).
- Polish failures: the AFC RESET lane-picker modal **wrapped** a button to a second line; the **print preview went blank** after resume; an idle lane's **tube stayed drawn to the hub** after unload.

### Root cause (grounded against live logs + code, 2026-06-15)

Backends publish rich error/progress information that HelixScreen under-consumes:

| Backend/Klipper publishes | Channel | HelixScreen today |
|---|---|---|
| Full error text + fix instruction | gcode RESPOND stream `!! …` | consumed by `GcodeErrorRouter`, but **only CFS-coded errors are classified**; an uncoded `!!` (AFC jam, Happy Hare, generic) falls to a **swallowable transient toast** |
| Structured recovery dialogs | Klipper `action:prompt_*` | consumed (`ActionPromptManager`) — but AFC's jam path emits none |
| Per-step narration | gcode RESPOND `// …` (e.g. `AFC_Brush: Clean Nozzle`) | ignored; step list hardcoded |
| Coarse fault flag | `AFC.error_state`, per-lane `status` | read → rendered as bare **"Error"** |
| Printer-wide error/pause/shutdown | `webhooks.state`/`state_message`, `print_stats.message`, `pause_resume.is_paused` | not unified into surfacing |

Verified facts:
- AFC's `AFC_error.py:AFC_error()` emits the `!!` text + sets `error_state=True` + runs `PAUSE`. On a jam it emits **no `action:prompt`** (5 runout errors vs 40 prompt events, uncorrelated) → smart recovery buttons must be **HelixScreen's** job.
- HelixScreen **already** subscribes to `notify_gcode_response` twice: `ActionPromptManager` (parses `action:prompt`) and **`GcodeErrorRouter`** (`src/application/gcode_error_router.cpp`) which parses `!!`/`Error:` lines, translates CFS coded errors via `CfsErrorDecoder`, RPC-dedups, and **replays the latest `!!` from `server.gcode_store` on reconnect**.
- **Root cause of E1, exactly:** `GcodeErrorRouter::process_line` routes by Klipper *error code* — `key298`→toast+Recover, `key8xx`→modal, **everything else→deferred transient toast** (lines 381–408). The AFC jam is plain text with no `{"code":…}`, so it falls into the catch-all toast — fired mid-toolchange while AFC-panel toasts are suppressed → invisible. The router is **CFS-shaped**, not backend-agnostic.
- This is **not AFC-specific**: CFS is the only classified backend; AFC, Happy Hare, IFS, QIDI, and generic Klipper errors all fall through. Same root for `feedback_cfs_errors_via_respond_raw`. One evolution fixes all of them.
- **L0 therefore EVOLVES `GcodeErrorRouter` into the pluggable `ErrorCenter`** — do NOT build a second `notify_gcode_response` consumer (that would double-surface every error). Keep its replay/dedup/translation; replace the hardcoded code switch with severity + a per-backend classify hook.

---

## 2. Goals / Non-Goals

**Goals**
1. One backend-agnostic pipeline that surfaces errors from **any** source (all AMS backends + Klipper) with the **full, untruncated** message.
2. **Severity-driven** UX: critical interrupts with a modal + recovery actions; warnings toast; info goes to history; a persistent header badge means a dismissed critical error is never lost.
3. **Smart, precondition-aware recovery** — offer the action that actually applies (e.g. Unload before Eject), never a dead end.
4. **Accurate toolchange step/phase** display driven by real narration, not hardcoded guesses.
5. Fix the associated polish bugs.

**Non-Goals**
- No changes to AFC / Klipper / Kalico firmware.
- No rework of unrelated panels.
- No speculative per-backend adapters beyond AFC + the generic fallback in v1 (YAGNI; the interface makes the rest cheap).

---

## 3. Callout Registry (acceptance criteria)

Every item the user raised. Each is checked off individually by the layer noted.

| ID | Callout | Evidence | Layer |
|----|---------|----------|-------|
| **E1** | Critical jam never surfaced as a dialog; generic "Filament Runout" only | img2 | L0 |
| **E2** | AFC error text truncated mid-word ("upstream senso") | img3 | L0 |
| **E3** | Audit: does the AMS overlay mirror real sensor state mid-error? | img3 | L1 |
| **E4** | Bare "Error" / "Error state" with no detail | img4, img8 | L0 |
| **R1** | "Eject failed: Unload from toolhead first" — but no Unload offered | img10 | L1 |
| **R2** | AFC RESET lane-picker modal wraps 4th button; not responsive | img5 | L3 |
| **R3** | Generic Home/Recover/Abort; need smart context-aware recovery | img4, img8 | L1 |
| **S1** | Step shows "Feed filament" while actually purging | img6 | L2 |
| **S2** | Missing brush/clean (and cut/poop/kick) steps | img6 vs img9 | L2 |
| **P1** | Print preview render blank after resume | img7 | L3 |
| **P2** | Stale lane→hub tube after idle-lane unload | first report | L3 |

---

## 4. Architecture

Layered. L0 is generic and the first build target; L1–L3 build on it.

```
            ┌──────────────────────── Ingestors (WS bg thread) ────────────────────────┐
            │  GcodeResponseIngestor   KlipperStateIngestor   ActionPromptBridge        │
            │   (!! errors, // narr)    (webhooks/print_stats   (existing prompt mgr →   │
            │                            /pause_resume)           ErrorEvent+actions)     │
            └───────────────┬───────────────────┬──────────────────────┬────────────────┘
                            │  (ui_queue_update / tok.defer to main)    │
                            ▼                                           ▼
                  ┌─────────────────────────  ErrorCenter (singleton, main thread)  ─────────────┐
                  │  classify (adapter registry) → dedup/policy → ErrorEvent{severity,detail,…}  │
                  └───────────────┬───────────────────────────────────────────────┬─────────────┘
        CRITICAL  │                          WARNING │                        INFO │
                  ▼                                   ▼                             ▼
        RecoveryModal (ModalStack,           ToastManager + history          history only
        rendered via ActionPromptModal)      + header "!" badge (persists, re-opens last critical)
```

### 4.1 L0 — ErrorCenter core (generic)

**This is an evolution of the existing `GcodeErrorRouter`, not a new class.** Reuse what's already hardened; replace what's CFS-specific:

| Existing in `GcodeErrorRouter` | Fate in `ErrorCenter` |
|---|---|
| `notify_gcode_response` registration (`on_notify_gcode_response`) | **keep** — the one `!!` ingestor |
| `clean_error_text()` (CFS decode + heuristics) | **keep** — first classifier in the chain |
| `gcode_store` replay on reconnect (`on_connected`, `should_surface_replay`) | **keep** — folds into the new severity routing |
| RPC-correlation dedup (`rpc_error_correlation`) | **keep** |
| Hardcoded `key298`/`key8xx`/else routing switch (`process_line` 278–408) | **replace** with: classify → severity → presenter |
| `find_recovery()` CFS-only `RecoveryAction` table | **generalize** into the per-backend `recovery_actions()` hook + the struct below |

New work layered on top: a **severity** field, a **per-backend classify hook**, **Klipper-state ingestion** (`KlipperStateIngestor`), and the **persistent badge** presenter. The `ActionPromptManager`/`ActionPromptBridge` stays a separate ingestor feeding the same `ErrorEvent` model.

**Data model**

```cpp
enum class ErrorSeverity { INFO, WARNING, CRITICAL };
enum class ErrorSource   { GENERIC, KLIPPER, HEATER, AFC, CFS, IFS, QIDI, HAPPY_HARE, SNAPMAKER, ACE, TOOLCHANGER };

struct RecoveryAction {
    std::string label;                 // "Unload", "Resume", "Recover"
    std::string gcode;                 // or a std::function callback for UI-side flows
    ButtonStyle style;                 // primary | danger | neutral
    std::function<PreconditionResult()> precondition; // {ok} or {blocked, reason}
};

struct ErrorEvent {
    ErrorSource   source;
    ErrorSeverity severity;
    std::string   title;               // short, classified ("Toolhead jam")
    std::string   detail;              // FULL text, never truncated
    std::string   raw;                 // original line(s)
    std::string   dedup_key;           // collapse repeats
    uint64_t      timestamp;
    std::vector<RecoveryAction> recovery_actions;
    bool          sticky;              // keep badge lit until resolved
};
```

**Ingestors** (all run on the WS background thread; hand to main via `ui_queue_update`/`tok.defer` — never touch LVGL or `ErrorCenter` UI state directly; see §6):
- **GcodeResponseIngestor** — new `!!` branch alongside the existing `ActionPromptManager` hook on `notify_gcode_response`. `!!  …` → candidate error; `//  …` → narration (tagged `source` by prefix, routed to L2, **not** surfaced as an error).
- **KlipperStateIngestor** — observes `webhooks.state`/`state_message` (shutdown/error → CRITICAL), `print_stats.state`/`message`, `pause_resume.is_paused` transitions.
- **ActionPromptBridge** — wraps the existing prompt manager's output as an `ErrorEvent` whose `action:prompt` buttons become `RecoveryAction`s. AFC's *interactive* prompts and our *synthesized* recovery now render through one modal.

**Classifier registry** — `ErrorCenter` asks each registered adapter to `classify_error(raw, state)`. First match wins; assigns `severity`, `title`, `dedup_key`, `recovery_actions`. **Default policy** for an unmatched `!!`: CRITICAL if printing/paused, WARNING if idle. **Dedup** collapses repeats (the jam fired 5× → one modal with a repeat count).

**Presenters** (existing components demoted to renderers — no rewiring of their internals):
- **CRITICAL** → push a generic **RecoveryModal** through `ModalStack`, rendered by the existing `ActionPromptModal` (dynamic title + full detail + recovery buttons). Set the header `!` badge sticky.
- **WARNING** → `ToastManager` (`NOTIFY_WARNING_T`) + notification history.
- **INFO** → history only.
- **Header `!` badge** — already present in the print-status header; make it persistent and tappable to re-open the last unresolved CRITICAL `ErrorEvent`. Clears when `error_state`/pause resolves.
- The AFC-specific `AmsLoadingErrorModal` is **retired** in favor of RecoveryModal.

**Severity policy table**

| Condition | Severity | Surface |
|---|---|---|
| Klipper shutdown / `webhooks.state==error` | CRITICAL | modal + badge |
| Print paused by a backend error (`!!` + pause) | CRITICAL | modal + recovery + badge |
| Adapter-classified recoverable warning | WARNING | toast + history |
| Unmatched `!!` while idle | WARNING | toast + history |
| `//` narration, status strings | INFO | history / step UI (L2) |

### 4.2 L1 — Per-backend error/recovery adapters

The classifier/recovery hooks are added to the existing **`AmsBackend`** base so **every** backend participates:

```cpp
// AmsBackend (base)
virtual std::optional<ErrorEvent> classify_error(const RawError& raw, const AmsState& s) { return {}; }
virtual std::vector<RecoveryAction> recovery_actions(const ErrorEvent& e, const AmsState& s) { return {}; }
```

- **GenericAdapter** (always registered, lowest priority) — provides baseline behavior for any source with no specialized adapter: surface full `!!` text, map `pause_resume`/`print_stats` to Resume/Cancel actions. Guarantees CFS, IFS, QIDI, etc. get correct surfacing on day one (closes the known CFS gap).
- **AFC adapter** (`ams_backend_afc.cpp`) — first hand-tuned implementation:
  - Classify: toolhead jam/break (`handle_toolhead_runout` text), lane errors, hub errors.
  - Recovery actions for the jam, precondition-aware: **[Resume]** (after manual clear; `AFC_RESUME`), **[Unload]** (`AFC_UNLOAD` / `LANE_UNLOAD`, offered when toolhead loaded — closes R1), **[Recover]** (`AFC_RESET`). Eject carries precondition "toolhead unloaded," so we render Unload instead of a failing Eject (R3).
  - **E3 audit:** verify the AMS overlay path/sensor render reflects live sensor state during an error (cross-check against `AFC_stepper` sensors); fix if it lags.
- Roster to implement the hooks over time: AFC (v1) → CFS, IFS, QIDI, Happy Hare, Snapmaker, ACE, Toolchanger (later specs). Each is a small, isolated adapter.

### 4.3 L2 — Toolchange step/phase narration

Replace the hardcoded `ui_ams_sidebar.cpp` step list (Heat→Feed→Purge) with steps derived from the backend's real narration:
- A **narration parser** consumes the tagged `//` lines from L0's GcodeResponseIngestor (`AFC_Brush: Clean Nozzle`, `Move to Brush`, `lane N is now loaded in toolhead`, cut/poop/kick/brush/clean/purge).
- A **generic step-model interface** on `AmsBackend` maps narration → ordered phases; AFC provides the first map. Fixes S1 (mislabel) and S2 (missing brush/clean).

### 4.4 L3 — Polish sweep (isolated)

- **R2** — AFC RESET lane-picker modal: responsive width / wrap so the 4th lane button never spills to a second line.
- **P1** — print preview blank after resume (related to existing note `project_3d_mode_blank_mid_print`): re-establish the render on resume/toolchange.
- **P2** — stale lane→hub tube: refresh the per-slot path canvas on per-lane status change, not only on system `path_filament_segment`/`path_topology` subject changes (traced: `ui_panel_ams.cpp` `slots_version_observer_` → `refresh_slots()` repaints spools but not the path; route per-slot segment refresh through the same signal).

---

## 5. Testing strategy

Real tests (fail if the feature regresses), per CLAUDE.md:
- **L0:** feed synthetic `notify_gcode_response` `!!` lines + Klipper-state transitions through the ingestors; assert ErrorEvent severity/detail/dedup and the chosen presenter (modal vs toast vs history). The **mock backend** emits synthetic errors.
- **L1:** AFC adapter classification + recovery-action sets, including precondition gating (Unload offered when toolhead loaded; Eject blocked). The live jam is reproducible on the Voron for end-to-end verification.
- **L2:** narration → step-index mapping incl. brush/clean and the purge-vs-feed case.
- **L3:** P2 regression — idle-lane unload must clear the tube segment (failing test first).

---

## 6. Threading & lifecycle constraints (mandatory)

- Ingestors run on the WS/libhv background thread. They must **never** call `lv_subject_set_*`, touch widgets, or mutate `ErrorCenter` presenter state directly. Hand off via `ui_queue_update()` / `tok.defer()` ([L072]).
- `ErrorCenter` holds an `AsyncLifetimeGuard`; background callbacks capture `tok` and defer.
- No synchronous widget deletion inside queued/deferred callbacks (`safe_delete_deferred` / `lv_obj_delete_async` / `safe_clean_children`) ([L081]).
- Modal/toast presentation goes through `ModalStack` / `ToastManager` on the main thread only.

---

## 7. Sequencing

1. **L0** ErrorCenter core + ingestors + presenters + GenericAdapter — broad win, fixes E1/E2/E4 across all backends.
2. **L1** AFC adapter (+ E3 audit) — R1/R3.
3. **L2** narration steps — S1/S2.
4. **L3** polish — R2/P1/P2 (independent; can interleave).

Each layer is its own implementation plan (`writing-plans`). L3 items can be picked up opportunistically as warm-ups.

---

## 8. Open questions

- Exact dedup key for repeated `!!` (text hash vs source+title) — decide in L0 plan.
- Whether the header `!` badge should also aggregate WARNING count or only reflect CRITICAL — propose CRITICAL-only in v1.

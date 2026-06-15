# Spaghetti / Failed-Print Detection — `DetectionSource` Abstraction + Snapmaker U1 Adapter

**Date:** 2026-06-15
**Status:** Design — approved scope, pending spec review
**Scope (this doc):** A pluggable `DetectionSource` interface plus a single concrete adapter for the **stock Snapmaker U1**. Cloud (Obico/OctoEverywhere) and the Paxx `detect-http` poller are **explicitly deferred** — captured here only as extension points the interface must not preclude.

---

## 1. Motivation & framing

The Paxx "Extended Firmware" for the Snapmaker U1 popularized on-device AI spaghetti detection. Investigation showed the detection itself is **not portable** to HelixScreen's fleet and **not something HelixScreen should run in-process**:

- The detector is a YOLOv11 CNN quantized to `.rknn`, run on the U1's **Rockchip NPU** via Rockchip's `rknn_toolkit_lite2` runtime. The trained model weights are Snapmaker's proprietary stock assets (Paxx reuses, does not ship, them). Useless off that silicon.
- Most HelixScreen targets (AD5M ~14 MB memory diet, K1/AD5X MIPS32, CC1 armv7l) have **no NPU and no CPU headroom** for inference. An in-process TFLite detector would make the UI binary worse everywhere to help one printer slightly. Rejected.

**The reframe that defines the value prop:** every realistic detector (stock U1, Obico, OctoEverywhere) **already auto-pauses the print itself**. HelixScreen's job is therefore *not* to be the thing that pauses. It is to be the **detection-aware response surface**: explain *why* a print paused, show the offending camera frame, offer the right actions, and act as a **backstop** if the source's own pause fails.

## 2. The enabling finding (verified on-device)

On the **stock U1**, detection lives in Snapmaker's open-source Klipper fork (`klippy/extras/defect_detection.py`) and writes its result into a **custom `exception` field on the standard `print_stats` Moonraker object** — which HelixScreen already subscribes to. No MQTT, no Paxx firmware, no new dependencies.

Verified on the project U1 (192.168.30.103, stock firmware, Linux 6.1.99 aarch64) on 2026-06-15:

- `print_stats.py:get_status()` returns `'exception'` (line 332) and `'message'` (333) unconditionally. Live Moonraker query (`/printer/objects/query?print_stats`) returned:
  ```json
  {"print_stats": {"state": "standby", "exception": {}, "message": "", ...}}
  ```
  (`exception` empty when idle — present in the schema regardless.)
- `defect_detection.py` event path (lines 562–598): `code` is `0` default, `1`=dirty bed, `2`=**noodle/spaghetti** (`CONFIRM_NOODLE = 2`), `3`=residue, `4`=dirty nozzle. On confirmation it fires
  `print_stats:update_exception_info(id=532, …, code=code, level=2)` then
  `pause_resume.send_pause_command()` + `gcode.run_script("PAUSE\n")`.
- Detection event payload as HelixScreen observes it on the WS:
  ```jsonc
  {"print_stats": {
    "state": "paused",
    "exception": {"id": 532, "index": 0, "code": 2, "message": "detected noodle", "level": 2}
  }}
  ```
- Config/tune is a real Moonraker webhook: `defect_detection.py:125` registers `defect_detection/config` taking `noodle_enable` / `noodle_check_window` (clamped) / `noodle_sensitivity` (`high`/`low`). `[defect_detection]` is active in the device `printer.cfg`. Thresholds: low 0.83 / high 0.80, window 10.

**Open verification gap to close during implementation:** observe a *live* non-empty `exception` payload during an actual detection (idle device only showed `{}`). The source read is unambiguous, but confirming the exact serialized key set on a real fire is step 1 of implementation.

## 3. Non-goals (this iteration)

- No in-process or on-host ML inference (rejected, see §1).
- No cloud adapter (Obico/OctoEverywhere). Detection there is cloud-side; the only Moonraker-observable signal is a generic `state == "paused"` with **no reason and no score** — inference-only, deferred to §9.
- No Paxx `detect-http` (`:8091`) poller — narrow audience (Paxx-AI firmware only), unverified HTTP schema, and the stock `print_stats.exception` path already covers U1. Deferred to §9.
- HelixScreen does **not** own the primary pause for any source that self-pauses. Default policy is *defer-to-source*.

## 4. Architecture — `DetectionSource`

A narrow interface that normalizes "a print-failure detector said something" into one event type, decoupling HelixScreen's UI/policy from any backend.

```cpp
enum class DetectionKind { Spaghetti, DirtyBed, Residue, DirtyNozzle, Unknown };

struct DetectionEvent {
    std::string   source_id;       // "u1_stock"
    DetectionKind kind;            // mapped from exception.code
    bool          attributable;    // true: we KNOW the cause; false: inferred (cloud, future)
    std::optional<float> confidence;   // none for U1 stock (it only reports post-threshold)
    bool          already_paused;  // print_stats.state == "paused" when event seen
    std::string   message;         // raw source message, e.g. "detected noodle"
};

class DetectionSource {
public:
    virtual ~DetectionSource() = default;
    virtual std::string id() const = 0;
    virtual bool available() const = 0;            // gates UI visibility per-printer
    // Capabilities the UI/policy layer queries:
    virtual bool can_tune() const { return false; }   // U1: true (webhook)
    virtual bool self_pauses() const { return true; } // all current/known sources: true
    // Fires on the main thread (already marshaled) with a normalized event.
    using Callback = std::function<void(const DetectionEvent&)>;
    virtual void set_callback(Callback) = 0;
};
```

A `DetectionManager` (singleton, alongside the other `::instance()` managers) owns the registered source(s), applies policy (§6), and drives the response UI (§7). One source registered today; the vector/registry shape is deliberate so §9 sources slot in without touching the policy/UI layer.

`available()` is what makes this fleet-safe: on every non-U1 printer the source reports unavailable, the feature hides itself, and nothing ships dead weight to the MIPS targets.

## 5. Adapter — `U1StockSource`

- **`available()`**: true when the printer is identified as Snapmaker U1 **and** the live `print_stats` schema carries an `exception` key (cheap one-time probe of the already-streamed object; degrades to false elsewhere). Uses `PrinterDetector` capabilities for the U1 identity check.
- **Observation**: hooks the existing `print_stats` status-update path (where `printer_print_state` already parses `state`, `print_progress`, etc.). Reads the additional `exception` sub-object. No new subscription — `print_stats` is already subscribed.
- **Mapping**: `exception.code` → `DetectionKind` (2→Spaghetti, 1→DirtyBed, 3→Residue, 4→DirtyNozzle, else Unknown). `attributable = true`. `confidence = none` (stock reports only post-threshold confirmation). `already_paused = (state == "paused")`. Default surfaced kind for v1 is **Spaghetti only**; bed/residue/nozzle are parsed but may be notify-only or hidden behind a setting (they fire during their own calibration/maintenance flows, so they need UX care — see §8 risk).
- **Edge handling**: `exception` clears (`{}`) on print start/reset (`print_stats.py:142,156,162`). Treat a transition *into* a non-empty `exception` with a detection code as the event edge; ignore steady-state and clears. Debounce so a single detection raises one event, not one per status frame.
- **Tune (`can_tune() == true`)**: POST to Moonraker webhook `defect_detection/config` with `noodle_enable` / `noodle_sensitivity` (`high`/`low`) / `noodle_check_window`. Surfaced in the response modal and in settings.

## 6. Policy, de-dup, and backstop

Per-source policy setting: **`defer-to-source`** (default) / **`notify-only`** / **`backstop-pause`**.

- **De-dup is trivial** via `DetectionEvent.already_paused`. Because every current source self-pauses, when the event arrives the print is normally already `paused`. In `defer-to-source` and `notify-only`, HelixScreen **never issues its own pause** — it only surfaces.
- **Backstop (the one place HelixScreen acts):** if an *attributable* detection arrives (U1 spaghetti) but `state` has **not** reached `paused` within N seconds (default ~5 s; the source's own pause apparently failed), escalate via `AbortManager::start_abort()`. This is the genuine safety add — not redundant with the source.
- This keeps the "configurable per source with de-dup" decision intact while acknowledging that, for every backend that exists today, the honest default is defer + surface.

## 7. Response UI (all reused infrastructure)

On a surfaced detection:

- **Modal** (extends the existing `Modal` pattern, mirrors `ui_emergency_stop` / cancel-confirm): title from `kind` ("Spaghetti detected — print paused"), the **offending camera frame** (already decoded to RGB888 in `camera_stream.cpp`; grab the most-recent frame buffer under `AsyncLifetimeGuard`), reason text from `message`, and actions:
  - `[Resume]` → `moonraker_job_api::resume_print`
  - `[Abort]` → `AbortManager::start_abort` (or `cancel_print`)
  - `[Tune sensitivity]` (U1 only, `can_tune()`) → `defect_detection/config` webhook
- **Lightweight path** (notify-only policy, or non-spaghetti kinds): `ToastManager::show(WARNING/ERROR, …)` with an action button to open the modal.
- Strings: spaghetti/noodle, "Snapmaker"/"Klipper"/"Moonraker" follow i18n rules ([L070] — product names not translated; surrounding sentence is).

## 8. Settings

Under a Detection section: master on/off; per-source policy (§6); spaghetti-only vs. all-defect-kinds; notify-vs-modal; and U1 tune controls (enable, sensitivity high/low, check window) writing through `defect_detection/config`. Settings persist via `SettingsManager`.

**Risk to design around:** codes 1/3/4 (bed/residue/nozzle) fire during the U1's *own* calibration/maintenance routines (`DEFECT_DETECTION_DETECT_BED`, nozzle stages in `defect_detection.py`), not only mid-print failures. v1 surfaces **spaghetti (code 2) during active printing only**; other codes are parsed but gated off by default to avoid false alarms during calibration. Confirm behavior against a live device before enabling them.

## 9. Deferred extension points (must remain expressible by the interface)

- **`CloudPauseInferenceSource`** (Obico / OctoEverywhere): detection is cloud-side; both auto-pause via Moonraker's normal pause API. Only observable signal is `state == "paused"` with no attribution/score. Would emit `DetectionEvent{attributable=false, confidence=none}` on an *unexplained* pause (no known M600/runout/manual/macro cause) while a cloud detector is the configured integration. Honest, weak, opt-in. The `attributable` flag and "unexplained pause" classifier are the only new machinery it needs — interface already accommodates it.
- **`PaxxHttpSource`** (`:8091` / nginx `/detect-http/`): poll for live pre-pause confidence on Paxx-AI firmware. Needs on-device HTTP schema verification first; HTTP via `HttpExecutor`, never raw threads. Pure addition — no policy/UI change.

## 10. Threading & safety (mandatory)

- All detection events originate on the Moonraker **WS background thread**. Marshal to main before any LVGL/member access: `tok.defer(...)` with `AsyncLifetimeGuard`, never bare `[this]` ([L072]). The `DetectionSource::Callback` contract is "fires on main thread, already marshaled."
- Camera frame access for the modal: the frame lives on the camera background thread's double-buffer; copy under `AsyncLifetimeGuard` / the existing `camera_stream` delivery guard, no UAF across teardown.
- Any future HTTP poll (§9 Paxx) uses `HttpExecutor::fast()`, never `std::thread(...).detach()` ([L083]).
- Subjects (if any introduced for detection state) self-register cleanup via `StaticSubjectRegistry::register_deinit` inside `init_subjects()`.

## 11. Implementation outline (detailed plan via writing-plans)

1. **Verify a live fire on-device**: induce/observe a real non-empty `print_stats.exception` (code 2) on the U1 and capture the exact serialized payload; confirm the pause edge and webhook round-trip.
2. `DetectionSource` interface + `DetectionEvent` + `DetectionManager` skeleton (registry, no-op when empty).
3. `U1StockSource`: `available()` probe, `exception` parsing hooked into the existing `print_stats` path, edge/debounce, kind mapping, callback marshaling.
4. Policy + de-dup + backstop timer (`AbortManager` integration).
5. Response modal + toast + actions (resume/abort/tune), reusing existing modal/job-api/abort/toast infra.
6. Settings (master, per-source policy, spaghetti-only default, tune controls).
7. Tests: kind-mapping table, edge/debounce (no duplicate events, clear handling), de-dup (`already_paused` suppresses self-pause), backstop fires only when pause is absent past timeout. Real assertions — fail if the behavior regresses.
8. On-device validation on the U1; feature hidden/verified inert on a non-U1 target.

## 12. Risks & open questions

- **Live payload not yet observed** (idle only) — step 1 closes it. Low risk; the source is unambiguous.
- **Non-spaghetti codes during calibration** — gated off by default (§8); needs live confirmation before exposure.
- **U1-as-remote-screen coexistence**: HelixScreen runs alongside stock firmware; both observe the same pause. Since HelixScreen defers by default, no conflict — but confirm the stock Snapmaker UI and HelixScreen don't double-prompt confusingly.
- **MQTT `:1883` path intentionally unused** — richer per-frame data exists there, but it adds a dependency and access-control unknowns for marginal gain over `print_stats.exception`. Not worth it for v1.

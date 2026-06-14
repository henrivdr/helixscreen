# Wizard Step Registry Refactor

**Status:** approved design, implementation pending
**Branch:** `feature/wizard-step-registry`
**Date:** 2026-06-14

## Problem

The setup wizard identifies and dispatches steps by a bare `int` index (0–12). The
architecture map found **45 distinct sites that branch on the raw step integer**,
including two 13-case `switch` statements (`ui_wizard_load_screen`,
`ui_wizard_cleanup_current_screen`) and ~21 scattered `if (next_step == N ...)`
checks in `on_next_clicked` / `on_back_clicked`. There is:

- **No base class** — the 13 step classes are independent, duck-typed
  (`init_subjects/register_callbacks/create/cleanup/get_name`, plus an *optional*
  `should_skip()` / `is_validated()`).
- **Two parallel skip mechanisms** — 13 file-static `*_step_skipped` bools updated
  in several places, AND 6 `should_skip()` methods, with the preset decision split
  into a central `if/else` in `precalculate_skips()`. These disagreed: the
  `on_next_clicked` cascade gated heater/fan on the static flags but re-ran
  `should_skip()` (live-hardware only) for AMS/LED/input-shaper, so preset printers
  with that hardware (Creality K2 Plus: CFS + LEDs + accelerometer) were marched
  through steps the preset already covered. (Interim fix: commit `1176e8074`.)
- **No explicit ordering** — order is implicit in array indices, duplicated across
  ~8 places. Adding/removing/reordering a step is error-prone. "Step 10" is
  meaningless at every call site.

## Goals

1. A `WizardStep` **abstract base class**; all 13 steps inherit it.
2. **Named** steps via a `WizardStepId` enum; the existing XML component string
   (`"wizard_heater_select"`) doubles as the human/log name. No raw integers in
   wizard control flow.
3. A single **ordered registry** — the one place that defines step order + identity.
4. **Per-step skip policy**: each step decides via `should_skip(const WizardContext&)`,
   preset-aware. Navigation walks the registry asking each step → supports
   non-linear skipping. The 13 static bools and the central preset `if/else` are
   deleted.
5. Skip + navigation logic becomes **unit-testable** (operates on the registry +
   plain-data `WizardContext`, no LVGL).

## Non-goals

- No change to any step's XML, visuals, or per-step UI behavior.
- No change to the preset *policy* itself — reuse the existing, tested
  `helix::wizard_preset_plan(has_preset, printer_count) -> {skip_hardware, first_run}`.
- No change to telemetry/summary semantics beyond expressing them as per-step skips.

## Decisions (approved)

- **Identity:** `WizardStepId` enum for type-safe identity; reuse the XML component
  string as the log/human name. No separate string table.
- **Scope:** full migration of all 45 integer-branch sites in this worktree
  (half-migrated = two coexisting mechanisms = worse).
- **Ownership:** keep the existing lazily-created `get_wizard_*_step()` singletons
  (with their `StaticPanelRegistry` cleanup); the registry holds an ordered list of
  `WizardStep*` pointers to them. Minimal lifetime churn.

## Design

### Base class (`include/wizard_step.h`, new)

```cpp
namespace helix::wizard {

enum class StepId {
    TouchCalibration, Language, Wifi, Connection, PrinterIdentify,
    HeaterSelect, FanSelect, AmsIdentify, LedSelect, FilamentSensor,
    InputShaper, Summary, Telemetry,
};

// Plain data needed to decide skips — no LVGL, constructible in tests.
struct StepContext {
    Config* config = nullptr;
    MoonrakerAPI* api = nullptr;          // hardware() for presence checks
    WizardPresetPlan preset{};            // {skip_hardware, first_run}
    bool is_subsequent_printer = false;   // get_printer_ids().size() > 1
    bool is_fbdev = false;
    bool force_language_step = false;      // existing g_force_language_step
};

class Step {
public:
    virtual ~Step() = default;
    virtual StepId id() const = 0;
    virtual const char* component_name() const = 0;  // "wizard_heater_select"
    virtual const char* log_name() const = 0;        // existing get_name()
    virtual bool should_skip(const StepContext&) const { return false; }
    virtual void init_subjects() = 0;
    virtual void register_callbacks() = 0;
    virtual lv_obj_t* create(lv_obj_t* parent) = 0;
    virtual void cleanup() = 0;
    virtual bool is_validated() const { return true; }
};

} // namespace helix::wizard
```

The 13 existing step classes inherit `helix::wizard::Step` and mark their existing
methods `override`. Their `get_name()` becomes `log_name()`; add `id()` and
`component_name()` (returning the constant already in `STEP_COMPONENT_NAMES`).
`should_skip()` signature changes to take `const StepContext&`.

### Registry (`src/ui/wizard_step_registry.{h,cpp}` or within `ui_wizard.cpp`)

An ordered `std::array<Step*, 13>` (or `std::vector<Step*>`) built once from the
`get_wizard_*_step()` singletons, in display order. This array is the **single
source of step order and identity**. Helpers:

```cpp
const std::vector<Step*>& wizard_steps();          // ordered registry
Step* wizard_step_by_id(StepId);                   // lookup
int   wizard_step_index(const Step*);              // position (internal use only)
```

### Pure navigation (`wizard_step_logic.{h,cpp}`, extend)

Replace `WizardSkipFlags` (struct of 13 named bools) with skip evaluation over the
registry + a `StepContext`. New pure functions (unit-tested, no LVGL):

```cpp
// All operate on an ordered list of (id, skipped) — caller computes `skipped`
// per step via Step::should_skip(ctx). Keeps these functions LVGL-free.
struct StepSkip { StepId id; bool skipped; };

int  wizard_visible_count(const std::vector<StepSkip>&);
int  wizard_display_number(StepId current, const std::vector<StepSkip>&); // 1-based
StepId wizard_next(StepId current, const std::vector<StepSkip>&);  // or a sentinel "done"
StepId wizard_prev(StepId current, const std::vector<StepSkip>&);
bool   wizard_is_last(StepId current, const std::vector<StepSkip>&);
```

Use `std::optional<StepId>` (or a `Done` sentinel) for past-the-end. The existing
`wizard_next_step(int, WizardSkipFlags)` tests migrate to id-based equivalents.

### Skip migration (each step's `should_skip(ctx)`)

| Step | should_skip(ctx) returns |
|------|--------------------------|
| TouchCalibration | `!ctx.is_fbdev || already_calibrated()` (existing logic) |
| Language | `(language already set) && !ctx.force_language_step` |
| Wifi | `is_android() || ctx.is_subsequent_printer` |
| Connection | `ctx.preset.first_run && already_connected` (existing) |
| PrinterIdentify | `ctx.preset.skip_hardware` |
| HeaterSelect | `ctx.preset.skip_hardware` |
| FanSelect | `ctx.preset.skip_hardware` |
| AmsIdentify | `ctx.preset.skip_hardware || no_ams_present()` |
| LedSelect | `ctx.preset.skip_hardware || no_leds(ctx.api)` |
| FilamentSensor | `ctx.preset.skip_hardware || fewer_than_two_standalone_sensors()` |
| InputShaper | `ctx.preset.skip_hardware ? preset_resonance_done(ctx) : no_accelerometer()` |
| Summary | `ctx.preset.first_run` |
| Telemetry | `!ctx.preset.first_run` |

Notes:
- `ctx.preset.skip_hardware` is true for ANY printer with a preset (first or Nth);
  `first_run` is first-printer-only (telemetry must not re-prompt). This is the
  existing `wizard_preset_plan` policy — unchanged.
- **FilamentSensor side effect:** the current single-sensor auto-config
  (`auto_configure_single_sensor()`) and `FilamentSensorManager` population run in
  the nav cascade today. Move auto-config into the FilamentSensor step's own
  pre-create/skip path so it still runs when the step is skipped for the
  <2-standalone-sensor case, but NOT when skipped by preset (the preset already
  configured sensors). Capture this in tests.
- **InputShaper factory flag** (`preset_resonance_done`): default true → resonance
  is skipped for preset printers. A preset may set `initial_resonance_compensation_run:
  false` (e.g. a factory-shipped image) to force calibration. Read from the active
  preset/config; default true when absent.

### Dispatch (collapse the two switches)

`ui_wizard_load_screen` / `ui_wizard_cleanup_current_screen` stop switching on the
int and instead resolve the current `Step*` and call
`step->init_subjects()/register_callbacks()/create()` and `step->cleanup()`.
`get_step_title_from_xml` uses `step->component_name()`. The `on_next_clicked` /
`on_back_clicked` cascades are replaced by a single `wizard_next/prev` walk over
the registry with per-step `should_skip(ctx)`.

`current_screen_step` becomes a `StepId` (or `Step*`), not an int. The
"navigate by step int" public entry points (`ui_wizard_navigate_to_step`) keep an
int boundary only where XML/callbacks require it, translated through the registry.

## Files touched

- **New:** `include/wizard_step.h` (base class, StepId, StepContext).
- **New/extended:** `wizard_step_logic.{h,cpp}` — id-based pure nav, drop
  `WizardSkipFlags` struct-of-bools.
- **Edit:** all 13 `include/ui_wizard_*.h` + `src/ui/ui_wizard_*.cpp` — inherit
  `Step`, add `id()/component_name()`, port `should_skip(ctx)`.
- **Edit:** `src/ui/ui_wizard.cpp` — registry, dispatch via polymorphism, delete the
  13 static bools + central preset `if/else` + 45 int branches; build `StepContext`
  once per navigation.
- **Edit:** `tests/unit/test_wizard_step_logic.cpp` — migrate to id-based nav +
  per-step skip tests, incl. the K2-Plus regression (subsequent preset printer skips
  ams/led/input_shaper, shows summary, no telemetry) and the filament auto-config
  side-effect.

## Test plan (test-first)

1. Pure nav over the registry: visible count, display numbering, next/prev with
   arbitrary skip patterns incl. **non-contiguous** skips (proves non-linear).
2. Per-step `should_skip(ctx)`: build `StepContext`s and assert each step's decision
   across {no preset, first-run preset, subsequent preset, hardware present/absent}.
3. Regression (the bug that started this): subsequent printer + preset →
   PrinterIdentify/Heater/Fan/Ams/Led/Filament/InputShaper all skipped, Summary
   shown, Telemetry skipped.
4. Filament single-sensor auto-config fires when skipped for <2 sensors, NOT when
   skipped by preset.
5. InputShaper factory flag: `initial_resonance_compensation_run=false` in preset →
   not skipped even under `skip_hardware`.

## Risks

- LVGL-coupled dispatch is hard to unit-test; mitigate by keeping all *decision*
  logic in the pure layer and making dispatch a thin polymorphic call.
- Step singletons are lazily created via `get_wizard_*_step()`; the registry must
  trigger creation in a defined order at wizard init, preserving existing
  `StaticPanelRegistry` teardown.
- `current_screen_step` is read in several places (progress display, back-button
  floor); all must move to StepId consistently — grep after migration for any
  remaining raw int step comparisons.

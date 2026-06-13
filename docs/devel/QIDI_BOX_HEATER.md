# QIDI Box Heater — Reverse Engineering Reference

Developer reference for the QIDI Box's PTC filament-drying heater. Documents Klipper object schema, G-code command surfaces, firmware variants, and HelixScreen's integration points. Written to preserve findings that required significant research so the next developer doesn't start from scratch.

**See also**: [FILAMENT_MANAGEMENT.md § Dryer / Box-Heater Control](FILAMENT_MANAGEMENT.md#dryer--box-heater-control)

---

## Klipper Object Layout

### Heater

The drying chamber is exposed as a standard Klipper `heater_generic` object:

```
heater_generic heater_box<N>
```

where `N` is typically `1` (first or only Box unit). A second chained unit would be `heater_box2`, and so on.

**Status fields** (readable via `printer.objects.query`):

| Field | Type | Description |
|-------|------|-------------|
| `temperature` | float | Current chamber temperature (°C) |
| `target` | float | Target setpoint (0 = heater off) |
| `power` | float | Heater PWM duty cycle (0.0–1.0) |

Because this is a standard `heater_generic`, the full Klipper heater safety system applies — the firmware will fault and shut down if temperature limits are exceeded.

### Humidity and Temperature Sensors

The Box units ship with an AHT20 combo sensor:

```
aht20_f heater_box<N>
```

**Status fields**:

| Field | Type | Description |
|-------|------|-------------|
| `temperature` | float | Chamber ambient temperature (°C) |
| `humidity` | float | Relative humidity (%) |

Additional backup thermistors are present in some firmware builds:

```
temperature_sensor heater_temp_a_box1
temperature_sensor heater_temp_b_box1
```

These are read-only sensors used for safety monitoring. HelixScreen reads humidity and chamber temp from the `aht20_f` object as part of `DryerInfo`.

### Drying State

The active drying session is tracked in two places — both carry the same data; HelixScreen reads `box_extras` as primary and falls back to `multi_color_controller` if needed:

**Primary** (via `box_extras` Klipper plugin):

```
box_extras.box_drying_state.box<N>
```

**Mirror** (via `multi_color_controller` plugin):

```
multi_color_controller.drying.box<N>
```

**Fields** (identical in both):

| Field | Type | Description |
|-------|------|-------------|
| `dry_state` | int | `1` = drying active, `0` = idle |
| `end_time` | int | Unix epoch (seconds) when the drying session ends |

There is **no native "remaining minutes" field**. Compute remaining time on the client side:

```cpp
int remaining_minutes = static_cast<int>((end_time - now_epoch()) / 60);
```

### Detection

QIDI Box presence is detected by the existence of `box_stepper slot<N>` objects in `printer.objects.list`:

```
box_stepper slot1
box_stepper slot2
box_stepper slot3
box_stepper slot4
```

Presence of `box_stepper slot*` → `AmsType::QIDI_BOX`.

**Important — QIDI Q2 has two heaters**: A QIDI Q2 has both a printer chamber heater (typically `heater_generic chamber`) and a Box drying heater (`heater_generic heater_box1`). Only `heater_box<N>` is the Box dryer. Do not confuse or merge these.

---

## Temperature Limits

There are three distinct temperature ceilings, each enforced at a different layer:

| Tier | Value | Enforcement |
|------|-------|-------------|
| Hardware safety ceiling | **100 °C** | Klipper firmware — heater faults and shuts down above this |
| Settable target ceiling | **90 °C** | `target_max_temp_heater_generic` config key — Klipper rejects `SET_HEATER_TEMPERATURE` calls above this |
| Per-material recommended | **~45–60 °C** | QIDI `drying.conf` per-material table — soft guidance only, not enforced |

### Config Key Variants

The settable target ceiling appears under different key names depending on firmware lineage:

| Firmware | Config section | Key |
|----------|---------------|-----|
| Official QIDI firmware | `[heater_generic heater_box1]` | `max_temp` |
| Community firmware | `[box_config box0]` | `target_max_temp_heater_generic` |

Both are readable via `printer.configfile.settings`. HelixScreen queries both spellings and takes whichever is non-zero, defaulting to 90 °C if neither is present.

### Per-Material Drying Table

The QIDI `drying.conf` defines per-material drying parameters. Columns as shipped:

| Column | Description | Typical values |
|--------|-------------|---------------|
| Type | Filament material name | PLA, PETG, ABS, TPU, PA, … |
| Bed temp | Bed temperature during drying | — |
| Chamber temp | Target drying temperature | 55–60 °C |
| Time | Drying duration | 720 min (12 h) default |

HelixScreen surfaces these as `DryingPreset` entries via `get_drying_presets()` so users can select a material and get sensible defaults without manual entry.

---

## G-code Command Surface

The Box heater exposes multiple command layers. Use them in preference order:

### 1. Standard Klipper heater_generic (always available)

```gcode
SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=<temp>
SET_HEATER_TEMPERATURE HEATER=heater_box1 TARGET=0    ; stop
```

Works on all firmware variants because `heater_box1` is a standard `heater_generic`. The heater holds the temperature indefinitely — there is no firmware-managed timer when using this command.

### 2. Vendor drying session with firmware timer (box_extras.so)

```gcode
ENABLE_BOX_DRY BOX=<n> TEMP=<temp> END_TIME=<hours>
DISABLE_BOX_DRY BOX=<n>
```

Preferred when the `box_extras` plugin is present. The firmware manages the session timer, updates `box_drying_state.end_time`, and clears `dry_state` when the session ends. `END_TIME` is a duration in hours (e.g., `END_TIME=4` for a 4-hour session).

### 3. Vendor multi-color controller commands

```gcode
MULTI_COLOR_SET_TEMP              ; set box temperature
MULTI_COLOR_DISABLE_HEATER        ; stop heater
MULTI_COLOR_DRY                   ; start drying session
```

Available when the `multi_color_controller` plugin is loaded. These are a higher-level wrapper around the box_extras primitives. Less preferred because parameter syntax is less well-documented.

### 4. Community wrappers (not on stock firmware)

```gcode
TLTG_SET_BOX_TEMP BOX=<n> TARGET=<temp>
OPTIMIZED_DISABLE_BOX_HEATER
```

Present only when the community open-source firmware replacement is installed (see thelegendtubaguy repo below). Do not rely on these being available on user hardware.

---

## HelixScreen Implementation

### Backend Files

| File | Role |
|------|------|
| `src/printer/ams_backend_qidi.cpp` | `AmsBackendQidi` — dryer virtuals implementation |
| `include/ams_backend_qidi.h` | Class declaration |
| `include/ams_types.h` | `DryerInfo` struct |
| `src/printer/ams_state.cpp` | `AmsState::sync_dryer_from_backend()` — subject bridge |

### DryerInfo Population (`get_dryer_info`)

The `get_dryer_info()` implementation queries three sources:

1. **`heater_generic heater_box<N>`** — `temperature`, `target`, `power`
2. **`box_extras.box_drying_state.box<N>`** — `dry_state`, `end_time` → compute `remaining_minutes`
3. **`printer.configfile.settings`** — `max_temp` / `target_max_temp_heater_generic` → caps field in `DryerInfo`

Humidity comes from `aht20_f heater_box<N>`.

### Command Policy (`start_drying` / `stop_drying`)

```
If box_extras drying state is subscribed and present:
    → prefer ENABLE_BOX_DRY / DISABLE_BOX_DRY  (firmware owns timer)
Else:
    → fall back to SET_HEATER_TEMPERATURE  (manual hold, no timer)
```

This means the countdown display is only available on firmware that exposes `box_extras`. On firmware without `box_extras`, the heater runs until manually stopped.

### Live Subscriptions

`box_extras` and `save_variables` are included in the Moonraker object subscription sequence (in `moonraker_discovery_sequence.cpp`, gated on `AmsType::QIDI_BOX`). This keeps the drying countdown and `dry_state` updating live during a session without polling.

### Safety Gate — `HELIX_QIDI_BOX_WRITE`

All write operations in `start_drying()` and `stop_drying()` are gated behind an environment variable for field-testing safety:

```bash
HELIX_QIDI_BOX_WRITE=1 ./build/bin/helix-screen
```

When `HELIX_QIDI_BOX_WRITE` is not set, the dryer commands are logged at DEBUG level but nothing is sent to the printer. This prevents accidental hardware actuation while the firmware command syntax is still being validated against real hardware.

Remove this gate once the command syntax has been confirmed on real QIDI Q2 + Box hardware.

---

## Firmware Sources and References

| Source | URL | Notes |
|--------|-----|-------|
| Official QIDI Klipper fork | https://github.com/QIDITECH/klipper | `box_extras.py`, `aht20_f.py`, `multi_color_controller` module |
| Community RE + macros | https://github.com/thelegendtubaguy/Qidi-Max-4-Optimized | `config/box.cfg`, `config/drying.conf`, `docs/qidi_box/` — primary protocol reference |
| Community firmware replacement | https://github.com/qidi-community/Plus4-Wiki/tree/main/content/customisable_qidibox_firmware | Open-source replacements for six obfuscated `.so` modules |

---

## Known Unknowns and Field Validation

> **The exact vendor command and parameter syntax (`ENABLE_BOX_DRY`, `DISABLE_BOX_DRY`, `BOX=` numbering, `END_TIME` unit) is pending validation on real QIDI Q2 + Box hardware.** The syntax documented here is derived from firmware source inspection and community RE, not from a live device.

If firmware behavior differs from what is documented here, adjust the command string in `AmsBackendQidi::start_drying()` and `stop_drying()`. The rest of the pipeline (DryerInfo population, subscription, UI) does not need to change — only the emitted G-code.

Also unconfirmed:

- Box numbering when multiple Box units are chained (does `BOX=2` work as `heater_box2`?)
- Whether `END_TIME` is in hours or minutes on the PLUS4 firmware variant
- `MULTI_COLOR_DRY` parameter shapes
- Whether `box_extras` is present on all supported firmware versions or only some

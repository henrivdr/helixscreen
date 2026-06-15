# Printer Presets

Platform-specific default configurations for known-hardware builds.

## How Presets Work

For supported platforms (like AD5M and Snapmaker U1), the preset is baked into the release package as `config/settings.json` by the matching `release-<platform>` target in `mk/cross.mk` (see the `cp assets/config/presets/<platform>.json` line). Presets are not auto-discovered — adding a new one requires editing the relevant release target.

- **Fresh installs**: Preset is used, abbreviated wizard runs (language + telemetry only)
- **Upgrades**: Existing `settings.json` is preserved (backup/restore in installer)

The preset sets `wizard_completed: false` so the abbreviated wizard runs on first boot. The `preset` field triggers **preset mode**, which skips all hardware configuration steps since the answers are already known.

## Available Presets

| File | Platform | Notes |
|------|----------|-------|
| `ad5m.json` | Flashforge Adventurer 5M / 5M Pro | Touch calibration, hardware mappings, ForgeX macros |
| `ad5x.json` | Flashforge Adventurer 5X | Same hardware as AD5M, different display settings |
| `cc1.json` | Elegoo Centauri Carbon (COSMOS firmware) | Factory white-balance calibration (per-channel panel gain), hardware mappings, load-cell probe, Moonraker on port 80 |
| `artillery-m1-pro.json` | Artillery M1 Pro | Touch calibration, hardware mappings, sound disabled (CPU overload) |
| `voron-v2-afc.json` | Voron V2 with AFC | Reference config, not auto-baked |
| `qidi_q2.json` | Qidi Q2 + QIDI Box (Happy Hare) | Network-detected (applied by the wizard, not baked); hardware mappings + Happy Hare filament-sensor roles |
| `anycubic_kobra_2_pro.json` | Anycubic Kobra 2 Pro (Rinkhals) | Network-detected. Conservative: heaters/sensors only — fan/LED/filament-sensor object names unverified (no on-device build yet) |
| `anycubic_kobra_3.json` | Anycubic Kobra 3 / Kobra 3 V2 (Rinkhals) | Network-detected. Shared by both (identical hardware). ACE handled by AMS backend, not this preset |
| `anycubic_kobra_3_max.json` | Anycubic Kobra 3 Max (Rinkhals) | Network-detected. Conservative mappings; dual-Y + ACE printer |
| `anycubic_kobra_s1.json` | Anycubic Kobra S1 (Rinkhals) | Network-detected. Enclosed CoreXY; conservative mappings |
| `anycubic_kobra_s1_max.json` | Anycubic Kobra S1 Max (Rinkhals) | Network-detected. Heated chamber present but object name unverified — not mapped yet |

> **Anycubic/Rinkhals presets are intentionally minimal.** [Rinkhals](https://github.com/rinkhals-community/Rinkhals) overlays real Klipper+Moonraker on the Kobra series, so these printers are network-detected (no on-device HelixScreen build exists for the Rockchip RV1106 host). The presets map only the universal `heater_bed`/`extruder` objects plus a `cooldown` macro; fan, LED, heated-chamber, and ACE/filament-tracker object names need confirmation against a live Rinkhals device before they can be added (otherwise they would raise false missing-hardware warnings). The setup wizard still configures anything the preset omits.

## What's in a Preset

Presets contain only basic hardware configuration:

- **`preset`** - Platform identifier, triggers abbreviated wizard mode
- **`wizard_completed: false`** - Ensures abbreviated wizard runs on first boot
- **Touch calibration** (`input.calibration`) - Hardware-specific touch matrix
- **Display quirks** (`display.*`) - Platform-specific display driver settings
- **Hardware mappings** (`printer.heaters`, `fans`, `leds`, `filament_sensors`) - Klipper object names
- **Expected hardware** (`printer.hardware.expected`) - For missing hardware warnings
- **Default macros** (`printer.default_macros`) - Platform-specific G-code macros

What's NOT in presets:

- **`printer.moonraker_host` / `moonraker_port` / `moonraker_api_key`** - Deployment-specific connection settings, set by the Connection wizard step. Presets must NEVER carry these: `apply_preset_file()` merges preset content over the active printer, so a hardcoded host would clobber the user's entered IP (e.g. silently reverting a real LAN IP to `127.0.0.1`). The preset apply explicitly strips these keys.
- `printer.type` - Auto-detected from Klipper hardware fingerprints
- `hardware.last_snapshot` - Runtime data, populated on first connection
- `hardware.optional` - Runtime data
- `dark_mode`, `brightness`, `language` - User preferences, changeable in Settings

## Creating New Presets

1. Run through the setup wizard on the target hardware
2. Copy the generated `settings.json`
3. Add `"preset": "<platform>"` field
4. Set `"wizard_completed": false`
5. Remove: `printer.type`, `hardware.last_snapshot`, `hardware.optional`
6. Remove user preferences (`dark_mode`, `language`, etc.)
7. Remove sensitive data (API keys)
8. Save as `config/presets/<platform>.json`

9. Wire it up: in `mk/cross.mk`, in the matching `release-<platform>` target, add a `cp assets/config/presets/<platform>.json $(RELEASE_DIR)/helixscreen/config/settings.json` line right after the `cp -r ui_xml config` block. Mirror the pattern used by `release-ad5m`.

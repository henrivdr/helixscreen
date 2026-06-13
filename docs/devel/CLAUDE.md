# docs/devel/CLAUDE.md — Developer Documentation

All developer documentation lives here. When working on features, look up the relevant doc before guessing.

## Core Development

| Doc | When to read |
|-----|-------------|
| `DEVELOPMENT.md` | Build setup, dev environment, contributing |
| `ARCHITECTURE.md` | System design, component relationships, extended systems |
| `BUILD_SYSTEM.md` | Makefile internals, cross-compilation, patches |
| `TESTING.md` | Catch2 test infrastructure, test patterns |
| `LOGGING.md` | spdlog levels, when to use info vs debug vs trace |
| `COPYRIGHT_HEADERS.md` | SPDX license headers |
| `RELEASE_PROCESS.md` | Release workflow, versioning |
| `CI_CD_GUIDE.md` | CI pipeline, GitHub Actions |
| `ANDROID_PLAY_STORE.md` | Play Store publishing pipeline, one-time setup, promotion flow |

## UI & XML

| Doc | When to read |
|-----|-------------|
| `UI_CONTRIBUTOR_GUIDE.md` | **Start here** for UI/layout work: breakpoints, tokens, colors, widgets, layout overrides |
| `YOUR_FIRST_CONTRIBUTION.md` | Annotated walkthrough of a real settings overlay + pattern tour of AMS for bigger features |
| `CONTRIBUTOR_GOTCHAS.md` | Symptom-indexed "if you see X, you forgot Y" — silent-failure traps in XML, translations, subjects |
| `LVGL9_XML_GUIDE.md` | XML syntax, all widgets (ui_card, ui_button, ui_markdown, etc.), bindings |
| `DEVELOPER_QUICK_REFERENCE.md` | Quick code patterns: modals, CSV parser, layout, migration |
| `MODAL_SYSTEM.md` | ui_dialog, modal_button_row, Modal subclass pattern |
| `THEME_SYSTEM.md` | Theme internals: style architecture, theme_core C API, adding themed widgets |
| `THEME_CONTRIBUTOR_GUIDE.md` | For people **creating themes** — JSON schema, palette design, testing. No C++ needed. |
| `LAYOUT_SYSTEM.md` | Layout system internals: LayoutManager C++ API, auto-detection logic |
| `TRANSLATION_SYSTEM.md` | i18n: YAML strings -> code generation -> runtime lookups |
| `TRANSLATION_CONTRIBUTOR_GUIDE.md` | For **translators** — how to improve existing translations or add a new language. No code needed. |
| `UI_TESTING.md` | Headless LVGL testing, UITest utilities |
| `GCODE_VIEWER_CONFIG.md` | GCode viewer configuration |
| `BED_MESH_RENDERING_INTERNALS.md` | Bed mesh 3D rendering internals |
| `PRE_RENDERED_IMAGES.md` | Pre-rendered image pipeline |

## Feature Systems

| Doc | When to read |
|-----|-------------|
| `LABEL_PRINTER_SYSTEM.md` | Label printing: Brother QL, Phomemo, Niimbot, MakeID protocols; USB/TCP/Bluetooth transports |
| `FILAMENT_MANAGEMENT.md` | AMS, AFC (Box Turtle), Happy Hare, ACE (Anycubic ACE Pro), AD5X IFS, CFS, Tool Changer, multi-backend, dryer architecture |
| `QIDI_BOX_HEATER.md` | QIDI Box PTC heater RE reference: Klipper objects, G-code commands, firmware variants, HelixScreen integration |
| `FILAMENT_SLOT_METADATA.md` | Internal notes on `FilamentSlotOverrideStore`: per-backend integration, hardware-event clearing, lifetime discipline, local cache, legacy migration. Pair with `../specs/filament_slots.md` for the public wire format. |
| `plans/2026-02-15-spool-wizard-status.md` | Spool creation wizard: 3-step flow, API methods, visual test plan |
| `MULTI_EXTRUDER_TEMPERATURE.md` | Multi-extruder temperature tracking, ExtruderInfo, dynamic subjects |
| `TOOL_ABSTRACTION.md` | ToolState singleton, ToolInfo, tool-to-backend mapping, DetectState |
| `INPUT_SHAPER.md` | Calibration panels, frequency response charts, CSV parser, PID |
| `PREPRINT_PREDICTION.md` | ETA prediction engine, phase timing, weighted history |
| `EXCLUDE_OBJECTS.md` | Object exclusion, per-object thumbnails, slicer setup |
| `PRINT_STATE_MACHINE.md` | Print lifecycle state machine: states, transitions, guards, resource lifecycle |
| `PRINT_START_PROFILES.md` | Print start phase detection, JSON profiles |
| `PRINT_START_INTEGRATION.md` | User-facing macro setup for print start tracking |
| `UPDATE_SYSTEM.md` | Update channels (stable/beta/dev), R2 CDN, Moonraker updater |
| `SOUND_SYSTEM.md` | Audio architecture, JSON themes, backends (SDL, PWM, M300). User guide: `../user/guide/settings.md#sound-settings` |
| `LED_CONTROL.md` | LED control system: 5 backends, auto-state lighting, control/settings overlays, home panel widget |
| `PRINTER_MANAGER.md` | Printer overlay, custom images, inline name editing |
| `MULTI_PRINTER.md` | Multi-printer management: config v4, soft restart, printer switching |
| `TIMELAPSE.md` | Moonraker timelapse plugin integration |
| `CRASH_REPORTER.md` | Crash reporter: detection, delivery pipeline, CF Worker, modal UI |
| `CONFIG_MIGRATION.md` | Versioned config migration: adding new migrations, testing |
| `STANDARD_MACROS_SPEC.md` | Standard macro specifications |
| `MACROS_PANEL.md` | Macros panel architecture, parameter handling, home panel widgets |
| `POWER_BUTTON_HANDLING.md` | Power button behavior |

## Platform & Deployment

| Doc | When to read |
|-----|-------------|
| `INSTALLER.md` | Installation system, KIAUH extension, shell tests (bats) |
| `printers/CREALITY_K1_SUPPORT.md` | Creality K1 series platform (K1, K1C, K1 Max) |
| `printers/QIDI_SUPPORT.md` | QIDI platform (Q2 + Max 4 on-device; Plus 4 + older 3-series TJC models are remote-only) |
| `printers/SNAPMAKER_U1_SUPPORT.md` | Snapmaker U1 toolchanger platform |
| `printers/CREALITY_K2_SUPPORT.md` | Creality K2 series platform |
| `printers/FLASHFORGE_AD5X_SUPPORT.md` | FlashForge Adventurer 5X (MIPS, ZMOD) |
| `AD5M_KMOD_VARIANT.md` | Building HelixScreen as a native variant inside the AD5M Klipper Mod firmware |
| `ENVIRONMENT_VARIABLES.md` | All runtime and build env vars |

## Integration

| Doc | When to read |
|-----|-------------|
| `MOONRAKER_ARCHITECTURE.md` | Moonraker API abstraction, WebSocket integration |
| `PLUGIN_DEVELOPMENT.md` | Plugin API, lifecycle, UI injection, threading, examples |
| `TELEMETRY_ADMIN.md` | Telemetry pipeline, Analytics Engine, dashboard, scripts, secrets |

## Planning & Research

| Doc | When to read |
|-----|-------------|
| `ROADMAP.md` | Feature timeline, what's complete, what's next |
| `IDEAS.md` | Feature ideas and brainstorming |
| `plans/` | Active implementation plans |
| `plans/2026-02-23-xml-hot-reload.md` | XML hot reload: status, design decisions, Phase 3 stretch goal |
| `printer-research/` | Printer-specific research notes |
| `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | AD5X IFS protocol reverse engineering |
| `KLIPPERSCREEN_RESEARCH.md` | KlipperScreen competitive analysis |
| `MAINSAIL_RESEARCH.md` | Mainsail competitive analysis |

## Reference

| Doc | When to read |
|-----|-------------|
| `LVGL9_XML_ATTRIBUTES_REFERENCE.md` | Complete XML attribute reference |
| `LVGL9_XML_CHEATSHEET.html` | Quick XML cheatsheet (HTML) |
| `LVGL_XML_SITUATION.md` | LVGL XML licensing history and resolution (extracted to helix-xml) |
| `SLOT_COMPONENT_DESIGNS.md` | Slot component design patterns (ready to implement) |
| `plans/2026-02-18-helix-xml-plan.md` | Helix XML engine: extraction, upgrade & extension plan |
| `FLAG_ICONS_SOURCE.md` | Flag icon asset sources |
| `480x320_UI_AUDIT.md` | Small display UI audit |

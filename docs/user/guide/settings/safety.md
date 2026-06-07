# Settings: Safety & Notifications

The Safety & Notifications category covers emergency controls and print alert preferences.

---

## E-Stop Confirmation

| State | Behavior |
|-------|----------|
| **Off** (default) | Tapping E-Stop fires immediately — fastest emergency response |
| **On** | Shows a confirmation dialog requiring a tap-and-hold before triggering |

Enable this if you find yourself accidentally hitting the E-Stop button. Disable it if you need the fastest possible emergency response.

---

## Cancel Escalation

When a print cancel is sent, some printers take a long time to finish their cancel routine (parking tools, cooling down, running CANCEL_PRINT macros). Cancel Escalation adds a safety net: if the cancel doesn't complete within a timeout, HelixScreen automatically escalates to an emergency stop (M112).

| Setting | Options |
|---------|---------|
| **Cancel Escalation** | On/Off toggle. **Off by default.** |
| **Escalation Timeout** | 15, 30, 60, or 120 seconds. Only shown when escalation is enabled. Default: 30 seconds. |

**When to leave this off:**
- Toolchangers that need to park tools during cancel
- Printers with long CANCEL_PRINT macros
- Any printer where the cancel routine is expected to take more than a few seconds

**When to turn this on:**
- Simple printers where cancel should complete quickly
- If you've experienced "stuck" cancels where the printer never returns to idle

---

## Confirm before running macros

| State | Behavior |
|-------|----------|
| **Off** (default) | Tapping a macro button runs it immediately |
| **On** | Shows a confirmation dialog before running any macro |

Enable this if you have macros that move the toolhead, heat the printer, or perform other actions you don't want triggered by an accidental tap.

---

## Allow cold load/unload

| State | Behavior |
|-------|----------|
| **Off** (default) | Filament load/unload is blocked when the nozzle is below the minimum extrude temperature |
| **On** | Allows load/unload to run even when the nozzle is cold |

By default, HelixScreen won't run a filament load or unload while the nozzle is too cold to extrude, matching Klipper's cold-extrude safety check. Turn this on if your load/unload macros heat the nozzle themselves before extruding, so the operation isn't blocked before your macro gets a chance to warm up.

---

## Print Completion Alert

Controls how HelixScreen notifies you when a print finishes, is cancelled, or fails — when you're not already on the print status screen.

| Mode | Behavior |
|------|----------|
| **Off** | No visual notification (sound still plays if enabled) |
| **Notification** | Brief toast message at the top of the screen |
| **Alert** (default) | Full-screen modal showing print stats — duration, layers, filament used — with confetti for successful prints |

To change: **Settings > Safety & Notifications > Print Completion Alert** dropdown.

> **Note:** Print errors always show the full alert modal regardless of this setting, since errors need immediate visibility. If you're already on the print status screen when a print ends, no notification is shown (the panel itself shows the result).

Sound always plays for terminal print states (complete, cancelled, error) regardless of alert mode, as long as the master Sounds toggle is on.

---

[Back to Settings](../settings.md) | [Prev: Hardware & Devices](hardware.md) | [Next: System](system.md)

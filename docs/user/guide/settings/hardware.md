# Settings: Hardware & Devices

The Hardware & Devices category contains settings for printers, peripherals, and external services connected to your printer. Items in this section appear or hide dynamically based on what HelixScreen detects from Klipper and Moonraker.

---

## Hardware Issues

> Only shown when hardware issues are detected.

Tap to open the Hardware Issues overlay, which lists detected issues:

| Category | Meaning |
|----------|---------|
| **Critical** | Required hardware missing (e.g., no extruder heater) |
| **Warning** | Expected hardware not found (e.g., bed sensor disappeared) |
| **Info** | Newly discovered hardware that wasn't seen before |
| **Session** | Hardware changed since last session |

**Actions for non-critical issues:**

- **Ignore** — Mark as optional (won't warn again even if missing)
- **Save** — Add to expected list (will warn if it disappears later)

Use this when adding or removing hardware to keep HelixScreen's expectations accurate.

---

## Printers

> Only shown when [beta features](../beta-features.md) are enabled.

Manage all your configured printers. Tap to open the Printer Management overlay where you can:

- **Switch printers** — Tap any printer in the list to switch to it. HelixScreen disconnects from the current printer and connects to the new one.
- **Add a printer** — Tap "Add Printer" to launch the Setup Wizard for a new printer. You can cancel at any time to return to your current printer.
- **Delete a printer** — Tap the trash icon next to any non-active printer and confirm. You cannot delete the last remaining printer.

After switching, a toast notification confirms the new connection and you're taken to the Home panel.

---

## Multi-Filament System Management

> Only shown when a multi-filament system is detected.

Tap to open Device Operations for quick actions, calibration, and speed settings for multi-material systems. Supports AFC, Happy Hare, ACE, and other Klipper-based filament changers.

---

## Fans

> Only shown when fans are detected.

Tap to open the Fan Settings overlay. This shows all detected fans and their current speeds. You can rename any fan for easier identification — for example, rename "fan_generic exhaust_fan" to "Exhaust". Custom names appear everywhere fans are shown in the UI.

---

## Sensors

> Only shown when sensors are detected.

Tap to open the Sensor Settings overlay. Manage all detected filament sensors by choosing a role for each:

| Role | Behavior |
|------|----------|
| **Runout** | Pauses print when filament runs out |
| **Motion** | Detects filament movement (clog detection) |
| **Ignore** | Sensor present but not monitored |

Other detected sensors (accelerometers, probes, humidity, width, color) are listed as read-only information.

---

## LED Settings

> Only shown when LED hardware is detected.

Tap to open the full LED configuration overlay. This is a large topic with its own page — see [LED Settings](led-settings.md).

---

## Power Devices

> Only shown when Moonraker power devices are configured.

Tap to open the Power Devices overlay, where you can toggle individual power relays and smart outlets on or off. This is the same panel accessible from **Advanced > Power Devices** or by long-pressing the home panel power button.

See [Power Device Control](../advanced.md#power-device-control) for full details on device selection and the home panel quick-toggle.

---

## Spoolman

> Only shown when Spoolman is configured in Moonraker.

Tap to open Spoolman integration settings. HelixScreen connects to your Spoolman server for spool tracking, weight sync, and barcode scanning.

### Server Setup

If Spoolman is not yet configured, you'll see a setup screen. Enter the IP address and port of your Spoolman server, then tap **Connect**. HelixScreen verifies the connection and configures Moonraker automatically — no manual editing of `moonraker.conf` needed.

### Server Status

When connected, the settings screen shows your Spoolman server URL along with options to **Change** the server address or **Remove** the configuration entirely.

### Sync with Spoolman

Toggle this on to enable automatic weight polling. When enabled, HelixScreen periodically queries Spoolman for spool weight updates and displays the remaining filament on the home panel and filament panel.

### Refresh Interval

Controls how often HelixScreen polls Spoolman for weight updates. Options: **30 seconds**, **1 minute**, **2 minutes**, or **5 minutes**. Shorter intervals give more up-to-date readings but generate more network traffic.

### Barcode Scanner

Configure which USB device to use as a barcode scanner for scanning Spoolman QR codes on spool labels. By default, HelixScreen auto-detects scanners by looking for devices with "barcode" or "scanner" in their name.

If your scanner uses a generic name (e.g., "TMS HIDKeyBoard"), tap this setting to manually select it from a list of connected USB HID devices. The selection is saved and persists across restarts.

### Label Printer

Opens label printer configuration for printing spool labels with QR codes. See [Label Printing](../label-printing.md) for full setup instructions and supported printers.

---

[Back to Settings](../settings.md) | [Prev: Printing](printing.md) | [Next: Safety & Notifications](safety.md)

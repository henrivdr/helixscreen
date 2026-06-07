# Settings: Help & About

---

## Help & About

Four items in the Help & About category:

| Action | What It Does |
|--------|--------------|
| **Upload Debug Bundle** | Collects logs and system info for support (see below) |
| **Discord Community** | Join **discord.gg/helixscreen** for community help and feedback |
| **Documentation** | Visit **helixscreen.org/docs** for guides and reference |

### Debug Bundles

When you need help troubleshooting an issue:

1. Tap **Upload Debug Bundle** in Settings
2. The bundle collects your logs, system info, and configuration (no personal data)
3. Tap **Upload** to send the bundle securely
4. Share the resulting code with the HelixScreen team on Discord or GitHub

Debug bundles include:

- **System logs** — recent HelixScreen log output
- **Configuration** — your settings (sanitized, no passwords or API keys)
- **System info** — OS version, hardware details, display resolution
- **Crash data** — if a crash occurred, the crash report and backtrace
- **Crash history** — past crash submissions with their GitHub issue references (helps support identify recurring issues)
- **Device identifier** — a double-hashed ID used only for correlating telemetry data (not personally identifiable)

Debug bundles contain only technical information needed for troubleshooting — no passwords, API keys, or personal data.

---

## About

Tap the **About** row at the bottom of the Settings panel to open the About overlay. This sub-overlay shows system information, update management, and HelixScreen branding.

| Item | Description |
|------|-------------|
| **HelixScreen Logo & Branding** | HelixScreen logo, "by Preston Brown", copyright notice, and a scrolling contributor marquee |
| **Printer Name** | The name of your connected printer (set during setup wizard) |
| **Current Version** | Your installed HelixScreen version |
| **Update Channel** | Stable, Beta, or Dev — only visible when beta features are enabled |
| **Check for Updates** | Check for and install new versions (hidden on Android) |
| **Klipper** | Installed Klipper version (fetched from Moonraker) |
| **Moonraker** | Installed Moonraker version |
| **OS** | Operating system version |
| **Print Hours** | Total print hours tracked — tap to open the [History Dashboard](../advanced.md) |
| **Open Source Licenses** | View licenses for all open source libraries used by HelixScreen |

### Checking for Updates

Tap **Check for Updates** to look for a newer release on your selected [update channel](#update-channels). If one is available, an update dialog walks you through installing it. You'll see the following stages:

1. **Update Available** — Shows the new version. Tap **Install** to begin, or **Cancel** to dismiss.
2. **Downloading...** — A progress bar tracks the download. You can still **Cancel** at this point.
3. **Verifying...** — HelixScreen checks the downloaded file before installing.
4. **Installing...** — The new version is written into place. **Do not power off your printer** while this is in progress.
5. **Update installed!** — Confirmation that the new version is in place.
6. **Hang on, we'll be right back!** — HelixScreen restarts itself to run the new version.

If something goes wrong, an **Update Failed** screen appears with a **Retry** button so you can try again, or **Close** to dismiss.

> **Caution:** Once installation begins, leave the printer powered on until HelixScreen restarts on its own. Interrupting an install can leave HelixScreen in an inconsistent state.

This row is hidden on Android, where updates are managed through the Play Store.

### Easter Eggs

- Tap the **Printer Name** row **seven times** to launch a hidden Snake game
- Tap the **Current Version** row **seven times** to toggle beta features — works like Android's "tap build number" developer mode

### Enabling Beta Features

Tap the **Current Version** row seven times in Settings → About to toggle beta features.

When beta features are enabled:
- **Update Channel** selector appears (Stable / Beta / Dev)
- Additional items appear in the Advanced panel (Macro Browser, Timelapse, etc.)
- **Plugins** section appears in Settings
- Tap seven more times to disable

### Update Channels

| Channel | Description |
|---------|-------------|
| **Stable** | Recommended. Tested releases only. |
| **Beta** | Preview builds with new features. May have rough edges. |
| **Dev** | Development builds. Requires a `dev_url` configured under the `update` section of your config file. |

> **Note:** Selecting the **Dev** channel without a `dev_url` set in your configuration shows a "Dev channel requires dev_url in config" message and won't check for updates. Dev builds are intended for HelixScreen contributors — most users should stay on **Stable** or **Beta**.

---

[Back to Settings](../settings.md) | [Prev: System](system.md) | [Next: LED Settings](led-settings.md)

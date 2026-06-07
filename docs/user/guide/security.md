# Security & Screen Lock

HelixScreen can lock the screen behind a numeric PIN to prevent unauthorized access to your printer controls. Set a 4–6 digit PIN, optionally lock automatically when the display goes to sleep, and unlock with an on-screen keypad.

Open the Security overlay from **Settings > System > Security**.

---

## Setting a PIN

When no PIN is configured, the only option shown is **Set PIN**:

1. Tap **Set PIN**.
2. Enter a 4–6 digit numeric PIN.
3. Enter the same PIN again to confirm.

PINs must be numeric and between 4 and 6 digits — other entries are rejected. Once a PIN is set, the Change PIN, Remove PIN, and Auto-lock options appear.

---

## Changing or Removing a PIN

When a PIN is already set:

- **Change PIN** — enter your current PIN first, then enter and confirm a new one.
- **Remove PIN** — enter your current PIN to confirm, then the PIN is cleared and the lock is disabled.

Both actions require the current PIN, so someone who doesn't know it can't change or remove it.

---

## Auto-lock

The **Auto-lock** toggle (shown only when a PIN is set) ties the screen lock to the display sleep timeout. When auto-lock is on, the screen locks automatically as the display wakes from sleep or screensaver/dim — so the printer is protected whenever it's been idle. You'll need to enter your PIN to use the screen again.

> The display sleep timeout itself is configured under display settings. Auto-lock simply hooks into that same idle behaviour rather than having its own separate timer.

You can also lock the screen manually at any time using the lock widget on the Home dashboard, if it's enabled in your home widgets.

---

## While the Screen Is Locked

When locked, a full-screen overlay covers the UI with a numeric keypad:

- Tap the digits to enter your PIN; dots fill in as you type.
- Tap the **checkmark** to confirm and unlock.
- Use the **backspace** key to correct a mistake.
- If the PIN is wrong, a brief error message appears and you can try again.

**Emergency Stop stays available.** While a print is running, a red Emergency Stop button is shown in the top-right corner of the lock screen, so you can always halt the printer in an emergency without unlocking it first.

---

## Storage & Factory Reset

Your PIN is never stored in plain text — only a one-way hash is saved, so the actual digits can't be recovered from your settings.

A **Factory Reset** (Settings > System > Factory Reset) clears all HelixScreen settings, which includes your security PIN and the auto-lock preference. After a reset, the screen lock is disabled until you set a new PIN.

---

**Next:** [Camera](camera.md) | **Prev:** [Sensors](sensors.md) | [Back to User Guide](../USER_GUIDE.md)

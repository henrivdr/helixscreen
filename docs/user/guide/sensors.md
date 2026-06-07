# Sensors

HelixScreen discovers the sensors your printer reports to Klipper and groups them by type. Filament switch and motion sensors are configurable — you assign each one a role and choose whether it's monitored. The remaining sensor types are shown as read-only information.

Open the Sensor Settings overlay from **Settings > Hardware & Devices > Sensors**.

---

## Switch Sensors (Filament Runout & Motion)

The **Switch Sensors** section lists standalone filament sensors — both switch-style runout sensors and motion (encoder) sensors. Sensors that belong to a multi-material system (AMS/CFS) are filtered out and managed by that system instead.

### Master toggle

An **Enable Monitoring** switch at the top of the section turns filament sensor monitoring on or off for all switch sensors at once.

### Per-sensor role

Each sensor row has a role dropdown. Choose how that sensor is used:

| Role | Meaning |
|------|---------|
| **None** | Sensor discovered but not assigned — not monitored |
| **Runout** | Primary runout detection — used to detect filament running out |
| **Toolhead** | Toolhead/nozzle proximity sensor |
| **Entry** | Entry-point detection sensor |

Each sensor also has an enable toggle, which appears once a role other than **None** is assigned. Changing a role or toggle saves immediately.

> The sensor's hardware kind (switch vs. motion) is detected automatically and shown on the row — it isn't something you set. Roles are what you assign.

---

## Read-Only Sensor Types

The remaining sections list sensors for information only — there's nothing to configure here. A section only appears when at least one sensor of that type is detected.

| Section | What it covers | Type labels shown |
|---------|----------------|-------------------|
| **Probe Sensors** | Z probes for bed leveling and mesh | BLTouch, Smart Effector, Eddy, Probe |
| **Width Sensors** | Filament diameter sensors for flow compensation | TSL1401CL, Hall |
| **Humidity Sensors** | Chamber and dryer humidity monitoring | BME280, HTU21D |
| **Accelerometers** | Input shaper calibration sensors | ADXL345, LIS2DW, LIS3DH, MPU9250, ICM20948 |
| **Color Sensors** | TD-1 filament color detection | TD-1 |
| **Temperature Sensors** | MCU, host, and auxiliary temperature monitoring | MCU, Host, Aux |

> Width sensors also expose an enable toggle and a flow-compensation role dropdown in their rows, but probes, humidity, accelerometer, color, and temperature sensors are display-only.

---

## Chamber Assignment

Inside the **Temperature Sensors** section, two dropdowns let you override auto-detection if your chamber heater or sensor isn't named "chamber":

- **Chamber Heater** — pick which generic heater is your chamber heater (or leave on **Auto**, or disable with **None**).
- **Chamber Sensor** — pick which temperature sensor reports chamber temperature (Auto / a specific sensor / None).

The **Auto** option shows the currently detected name in parentheses, or "none detected" when nothing matched.

---

**Next:** [Security & Screen Lock](security.md) | **Prev:** [Fans](fans.md) | [Back to User Guide](../USER_GUIDE.md)

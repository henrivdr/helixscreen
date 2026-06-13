# AnyCubic CoreXY Printers with Moonraker Support

**Date**: 2026-02-02
**Status**: Comprehensive research complete

## Executive Summary

The most relevant AnyCubic CoreXY printers are the **Kobra S1 Combo** and **Kobra 3 Combo** series. They run **KobraOS** (Klipper rewritten in Golang) and can be extended with **Rinkhals** custom firmware for standard Moonraker. **HelixScreen already has ACE backend support** for the ACE Pro multi-material system.

---

## 1. Hardware Specifications

### Kobra 3 / Kobra 3 Combo (Most Popular)

| Component | Specification |
|-----------|---------------|
| **Processor** | Rockchip RV1106G3 (ARM Cortex-A7 @ 1.2GHz) |
| **RAM** | 256MB DDR3L (integrated) |
| **Storage** | 8GB eMMC |
| **Mainboard** | Trigorilla Spe B v1.1.x |
| **MCU** | Huada HC32F460 (stepper control) |
| **Display** | 4.3" Capacitive Touchscreen (tilting) |
| **Connectivity** | WiFi (2.4GHz), USB-A (3 ports) |
| **Build Volume** | 250 x 250 x 260mm |
| **Max Speed** | 600mm/s |
| **Hotend** | 300°C max |

### Kobra S1 / Kobra S1 Combo (CoreXY, Enclosed)

| Component | Specification |
|-----------|---------------|
| **Motion** | CoreXY (enclosed) |
| **Build Volume** | 250 x 250 x 250mm |
| **Max Speed** | 600mm/s (300mm/s recommended) |
| **Acceleration** | 20,000 mm/s² |
| **Hotend** | 320°C max |
| **Multi-Color** | Up to 8 colors (2x ACE Pro) |
| **Price** | $549-$749 |

### Kobra S1 Max Combo (Large CoreXY)

| Component | Specification |
|-----------|---------------|
| **Build Volume** | 350 x 350 x 350mm |
| **Heated Chamber** | 65°C active heating |
| **Hotend** | 350°C max |
| **Multi-Color** | Up to 16 colors (ACE 2 Pro) |
| **Price** | $849-$1,099 |

### Rockchip RV1106G3 Details

| Feature | Specification |
|---------|---------------|
| **Architecture** | ARM Cortex-A7 (32-bit) |
| **Clock** | 1.2 GHz |
| **Memory** | Integrated 256MB DDR3L |
| **NPU** | 1.0 TOPS INT8 (AI features) |

---

## 2. Stock Firmware: KobraOS

### What KobraOS Is
- **Based on Klipper** but rewritten in **Golang** ("klipper-go") — this is the
  **"K3" software base**. The newer Kobra X moved to a **"K4" base that is a C++
  Klipper port** with mandatory RSA-signed firmware (Rinkhals-blocked). See
  [KLIPPER_FORKS_AND_PORTS.md](KLIPPER_FORKS_AND_PORTS.md) for the full Go→C++
  story and the GPL/clean-room analysis.
- **Closed source** with locked configs
- **LVGL-based UI** for touchscreen
- Configs in `/userdata/app/gk/config/`

### Official Klipper-go Release
[github.com/ANYCUBIC-3D/klipper-go](https://github.com/ANYCUBIC-3D/klipper-go)

Supports: Kobra 3, Kobra 3 V2, Kobra 3 Max, Kobra S1

**Limitations:**
- "Certain proprietary features not open source"
- Not compatible with standard Python Klipper ecosystem
- No standard Moonraker WebSocket subscriptions

---

## 3. Multi-Material System: ACE Pro

### Specifications

| Feature | Specification |
|---------|---------------|
| **Slots** | 4 per unit |
| **Max Capacity** | 8 colors (2 units), 16 (S1 Max) |
| **Dryer** | Dual 200W PTC, 55°C max |
| **RFID** | Auto filament detection |
| **Connection** | USB (CDC-ACM serial) |
| **Baud Rate** | 115200 |

### USB Identification
```
/dev/serial/by-id/usb-ANYCUBIC_ACE_0-if00
/dev/ttyACM0
```

### Communication Protocol
JSON-based serial protocol with framed messages:
```
Header: \377\252
Length: 2 bytes
Payload: JSON ({"id":2948,"method":"get_status"})
Checksum: 2 bytes
```

**Commands:**
- `get_status` - Query device state
- `drying` - Start drying
- `drying_stop` - Stop drying

---

## 4. Moonraker Availability

### Stock
**No standard Moonraker** - KobraOS uses proprietary communication.

### With Rinkhals
Full Moonraker support:

| Service | Port |
|---------|------|
| Mainsail | 80, 4409 |
| Fluidd | 4408 |
| SSH | 22 (root:rockchip) |
| Camera | mjpg-streamer |

---

## 5. Custom Firmware Options

### Rinkhals (Recommended)
**Repository**: [github.com/jbatonnet/Rinkhals](https://github.com/jbatonnet/Rinkhals)

| Feature | Status |
|---------|--------|
| Moonraker | Yes |
| Mainsail/Fluidd | Yes |
| SSH | root:rockchip |
| Stock UI | Preserved |
| Install | USB `.swu` file |

**Supported Printers:**
| Printer | Supported Firmware |
|---------|-------------------|
| Kobra 3 | 2.4.4.7, 2.4.5 |
| Kobra 2 Pro | 3.1.2.3, 3.1.4 |
| Kobra S1 | 2.5.8.8, 2.5.9.9, 2.6.0.0 |
| Kobra 3 Max | 2.5.1.3, 2.5.1.7 |
| Kobra 3 V2 | 1.1.0.1, 1.1.0.4 |
| Kobra S1 Max | 2.1.6 |

### DuckPro-Kobra3
**Repository**: [github.com/utkabobr/DuckPro-Kobra3](https://github.com/utkabobr/DuckPro-Kobra3)

- Modified Moonraker (emulates missing features)
- Python 3.11, Nginx, Fluidd/Mainsail

---

## 6. Klipper Configuration

### With Rinkhals
Standard printer.cfg access via Moonraker:
- `printer_mutable.cfg` - User-modifiable (editing voids support)

### Filament Hub Object
```ini
[filament_hub]
serial: /dev/serial/by-id/usb-ANYCUBIC_AMS-CDC_ACM_...
baud: 115200
max_volumes: 16
enable_rfid: 1
```

---

## 7. Display Interface

### Hardware
- **Size**: 4.3" capacitive touchscreen
- **Resolution**: ~480x320 or 480x272 (typical for 4.3")
- **Framework**: **LVGL** (confirmed by Rinkhals LVGL injection)
- **Framebuffer**: `/dev/fb0`

### For HelixScreen
- Framebuffer access available
- Touch via Linux input subsystem
- Must coexist with or replace KobraOS UI
- LVGL already in use - compatible base

---

## 8. Root Access

### With Rinkhals
- **SSH**: `root@<printer-ip>:22`
- **Password**: `rockchip`

### System Details
- **OS**: Linux 5.10.160 (ARMv7)
- **Device**: "Rockchip RV1106G IPC38"

---

## 9. HelixScreen Compatibility

### Existing ACE Support

HelixScreen already has a complete ACE backend:
- `include/ams_backend_ace.h`
- `src/printer/ams_backend_ace.cpp`

**Features:**
- Polls REST at 500ms intervals
- Thread-safe state caching
- Load/unload filament (`ACE_CHANGE_TOOL`)
- Dryer control (35-70°C)
- 4-slot hub topology

**REST Endpoints:**
- `GET /server/ace/info` - System info
- `GET /server/ace/status` - Current state
- `GET /server/ace/slots` - Slot details

### What Would Be Needed

1. **Rinkhals Installation** - Provides Moonraker
2. **klipper-go Differences** - May need REST polling for some objects
3. **Display Options**:
   - **External**: HelixScreen on Pi connected to Moonraker
   - **On-printer**: Replace stock UI (challenging - weak CPU/RAM)

### Resource Warning
> "Those printers are quite weak in terms of CPU and Memory. Every additional app/feature and client will make the experience slower and might crash."

---

## 10. ACE Klipper Drivers

### ValgACE (Primary — includes Moonraker component)
[github.com/agrloki/ValgACE](https://github.com/agrloki/ValgACE) - ACE Pro driver for any Klipper printer. Provides the `ace_status.py` Moonraker component with REST endpoints required by HelixScreen.

### Installation (ValgACE)
```bash
git clone https://github.com/agrloki/ValgACE.git
cd ValgACE && ./install.sh
```

Add to printer.cfg:
```ini
[include ace.cfg]
```

### G-code Commands

| Command | Description |
|---------|-------------|
| `ACE_CHANGE_TOOL TOOL=n` | Load slot n (-1 to unload) |
| `ACE_FEED LENGTH=x SPEED=y` | Feed filament |
| `ACE_RETRACT` | Retract filament |
| `ACE_START_DRYING TEMP=t DURATION=m` | Start drying |
| `ACE_STOP_DRYING` | Stop drying |
| `ACE_STATUS` | Query status |

### REST API Endpoints
- `GET /server/ace/info`
- `GET /server/ace/status`
- `GET /server/ace/slots`

### Alternative Drivers

| Project | Target |
|---------|--------|
| [ACEPROK1Max](https://github.com/swilsonnc/ACEPROK1Max) | Creality K1 Max |
| [ACEPROSV08](https://github.com/szkrisz/ACEPROSV08) | Sovol SV08 |

---

## 11. Community Resources

### Discord
- **Rinkhals**: [discord.gg/3mrANjpNJC](https://discord.gg/3mrANjpNJC)

### GitHub

| Repository | Purpose |
|------------|---------|
| [jbatonnet/Rinkhals](https://github.com/jbatonnet/Rinkhals) | Custom firmware |
| [utkabobr/DuckPro-Kobra3](https://github.com/utkabobr/DuckPro-Kobra3) | Alternative CFW |
| [ANYCUBIC-3D/klipper-go](https://github.com/ANYCUBIC-3D/klipper-go) | Official Golang Klipper |
| [agrloki/ValgACE](https://github.com/agrloki/ValgACE) | ACE Pro driver |
| [printers-for-people/ACEResearch](https://github.com/printers-for-people/ACEResearch) | ACE reverse engineering |

### Documentation
- **Rinkhals Docs**: [jbatonnet.github.io/Rinkhals](https://jbatonnet.github.io/Rinkhals/)

---

## Recommendations

### Target Configuration
- **AnyCubic Kobra S1 Combo** (CoreXY, enclosed)
- **Rinkhals firmware** for Moonraker
- **ACE Pro** already supported (install the ValgACE Moonraker component for REST access)

### Integration Path
1. **External HelixScreen** device (Pi + touchscreen)
2. Connect to Rinkhals Moonraker
3. ACE backend handles ACE Pro

### Challenges
- klipper-go WebSocket differences
- May need REST polling (like ACE does)
- Limited onboard resources for direct printer install

---

## Conclusion

AnyCubic Kobra S1 Combo is a good HelixScreen target:
- ARM architecture (Cortex-A7)
- Rinkhals provides Moonraker
- ACE Pro **already supported** via ACE backend
- Best approach: external HelixScreen device connecting to printer's Moonraker

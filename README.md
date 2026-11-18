# Klimare AC app for flipper zero FREE download

A Flipper Zero infrared remote app for **Klimaire PTAC** through-the-wall air conditioners
(KTHN series, Midea-built). Turn the unit on/off, switch between Cool / Heat / Fan, and
raise or lower the temperature with real + / − buttons.

## ⬇️ Free download

**[Download klimaire_remote.fap](dist/klimaire_remote.fap)** (click it, then click the download button)

## Install

1. Connect your Flipper to qFlipper (or the Flipper mobile app).
2. Copy `klimaire_remote.fap` to `SD Card/apps/Infrared/`.
3. On the Flipper: **Apps → Infrared → Klimaire PTAC**.

## Controls

| Button | Action |
|---|---|
| **OK** | Power on / off |
| **Up / Down** | Temperature +1 / −1 °F (62–86 °F), hold to scroll |
| **Left / Right** | Mode: Cool → Heat → Fan |
| **Hold OK** | Fan speed: Auto → Low → Med → High |
| **Hold Left** | Switch IR protocol A / B |
| **Back** | Exit (settings are saved) |

**First time:** point the Flipper at the unit and press **OK**. If it doesn't beep, hold
**Left** to switch to protocol B and try again. The app remembers your choice.

Every press sends the full setting (mode + temperature + fan), just like the original
Klimaire TNRC-N remote. If someone changes the unit with its own panel, the next press
from the Flipper sets it back to what's on the Flipper screen.

## Compatibility

- Built for Flipper API 88.2: official firmware (current release) and RogueMaster RM1117 or newer.
- If your firmware says the app is outdated, rebuild from source (below) against your firmware's SDK.
- Some Klimaire PTAC units have no IR receiver. If neither protocol works, check for a
  small dark receiver window near the control panel.

## Build from source

```
pip install ufbt
ufbt
```

The built app lands in `dist/klimaire_remote.fap`.

## How it works

Two Midea A/C infrared protocols are supported, since Klimaire PTACs are Midea-built:

- **A:** Midea 48-bit (temperature sent natively in °F)
- **B:** Midea/Coolix 24-bit (temperature sent in °C, rounded)

---

Unofficial project. Not affiliated with or endorsed by Klimaire. Use at your own risk.

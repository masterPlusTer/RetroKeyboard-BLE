# Flipper Zero – Vape Display Driver (I2C Bit-Bang)

This project allows a **Flipper Zero** to control a vape device display over **I2C**, using the **external GPIO pins (C0/C1)**.

The display is **not compatible with standard I2C drivers**.  
It requires **precise timing and a specific packet sequence**, discovered through logic analyzer captures.  
For this reason, the implementation uses **software (bit-banged) I2C** with Arduino-like timing.

---

## ⚠️ Important Notes

- This display **does NOT work with Flipper's hardware I2C HAL**
- It **will flicker** if refreshed incorrectly
- Stable operation requires:
  - Bit-banged I2C
  - Slower clock timing
  - Burst-style frame transmission (Arduino-style `show()`)

---

## Features

- Control of:
  - 7-segment digits (top and bottom)
  - Mono LED groups (UI bars, indicators, icons)
  - Full RGB halo (12 LEDs)
- Flicker-free rendering using burst frame transmission
- Step-based demo mode controlled by Flipper buttons
- On-screen UI showing current step and brightness

---

## Wiring

| Flipper Zero | Display |
|-------------|---------|
| C0          | SCL     |
| C1          | SDA     |
| GND         | GND     |
| 3V3         | VCC     |

### Pull-ups (required)
- SDA → 3.3V through **4.7kΩ**
- SCL → 3.3V through **4.7kΩ**

> Do **NOT** use 5V pull-ups.

---

## How It Works

### I2C Behavior
- Address: `0x3C`
- Data is sent as:
  1. 8 single-byte preamble writes
  2. Short delay
  3. One 145-byte payload

This sequence **must be repeated multiple times** to latch correctly.

### Flicker Fix (Arduino-style)
Instead of continuous refresh:
- The frame is sent **only when the content changes**
- Each update is sent **multiple times in a burst**
- This mimics the original Arduino behavior and prevents decay flicker

---

## Build Instructions

1. Copy the app source into your Flipper firmware tree:

```bash
applications_user/vape_display/vape_display_app.c
```

2. Build the app:

```bash
./fbt fap_vape_display
```

3. Install to Flipper (USB connected):

```bash
./fbt launch APPSRC=vape_display
```

---

## Controls (Demo Mode)

| Button        | Action |
|--------------|--------|
| RIGHT        | Next step |
| LEFT         | Previous step |
| UP           | Increase brightness |
| DOWN         | Decrease brightness |
| OK           | Re-render current step |
| BACK (press) | Turn all LEDs OFF |
| BACK (long)  | Exit app |

---

## Demo Steps

The demo cycles through multiple meaningful states, including:

- Displaying digits (1, 9, 0, 6)
- Filling UI segments
- Battery / droplet / bolt icons
- Halo colors:
  - Blue
  - Red
  - Green
  - White
  - Purple
  - Cyan
  - Yellow
- Mixed modes (digits + halo, icons + halo)

---

## Why Bit-Banging Is Required

This display:
- Is timing-sensitive
- Does not tolerate jitter
- Does not properly latch frames using hardware I2C

Using software I2C with controlled delays is the **only reliable method**.

---

## Project Status

- Reverse engineering: ✔️
- Stable rendering: ✔️
- Flicker-free output: ✔️
- Modular driver cleanup: 🚧 (future)

---

## Disclaimer

This project is for **educational and experimental purposes**.  
You are responsible for any hardware you connect to your Flipper Zero.

---

## Credits

- Reverse engineering via logic analyzer
- Inspired by original Arduino-based control code
- Developed and tested on Flipper Zero (GPIO C0/C1)

---

Enjoy hacking.

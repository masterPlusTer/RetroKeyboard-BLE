# HP KB-9970 PS/2 to Bluetooth LE Keyboard

**Giving a classic HP PS/2 keyboard a second life with an ESP32-C3 SuperMini, Bluetooth Low Energy, its original lock LEDs, and an experimental internal battery.**


<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/108b15ef-7ca0-43bc-b704-910b523aade9" /><img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/0c14edef-fe18-4188-a11f-4858b8b4b23d" />


> **Project status (November 2026):** Initial public release: **`v1.ino`**. The keyboard works over Bluetooth, including Windows lock LEDs, with an experimental internal battery and a CPU-based power-bank keep-alive. This is a working prototype, not yet a finished or endurance-tested device.

This project converts an **HP KB-9970 / Chicony PS/2 keyboard** into a **Bluetooth Low Energy (BLE) HID keyboard**, without replacing the original controller or reverse-engineering the complete key matrix. The original PS/2 controller scans the keys; an **ESP32-C3 SuperMini** acts as the PS/2 host and translates the keyboard's scan codes into Bluetooth HID reports.

The design targets the **PS/2 protocol**, not a particular Chicony chip. Other PS/2 keyboards *may* be adaptable, but voltage, scan-code sets, special keys, and LEDs must be verified individually.

## Contents

- [Project highlights](#project-highlights)
- [How it works](#how-it-works)
- [Hardware and wiring](#hardware-and-wiring)
- [Internal battery prototype](#internal-battery-prototype)
- [Firmware architecture](#firmware-architecture)
- [Build and upload](#build-and-upload)
- [Testing and platform behavior](#testing-and-platform-behavior)
- [Serial diagnostics](#serial-diagnostics)
- [Photo gallery](#photo-gallery)
- [Compatibility, limitations, and safety](#compatibility-limitations-and-safety)
- [Next steps](#next-steps)

## Project highlights

- Preserves the **original HP keyboard, Chicony controller, membrane matrix, keycaps, and three original status LEDs**.
- Reads **PS/2 Scan Code Set 2** using CLOCK/DATA and translates make/break sequences into BLE HID reports.
- Handles letters, numbers, modifiers, navigation, function keys, numeric keypad, and supported multimedia keys.
- Uses **bidirectional PS/2** to send `0xEE` (Echo) and `0xED` (Set LEDs), with PS/2 acknowledgment handling.
- Synchronizes **Caps Lock, Num Lock, and Scroll Lock** LEDs with the host-reported state when that host provides the corresponding LED states.
- Uses an **8 ms initial HID event gap**; diagnostic options allow testing 12 ms and 20 ms as well.
- Includes a **15-second task watchdog**, reset-cause reporting, PS/2 frame validation, event-queue diagnostics, and defensive recovery measures.
- Embeds a **single-cell Li-ion battery and IP5306-based USB power-bank module** inside the existing case (experimental).
- Includes an **experimental periodic CPU-load keep-alive** to discourage power-bank low-load auto-shutdown. Initial testing appeared promising, but current draw and long-term operation have not been measured.

## How it works

```text
  HP KB-9970 (original hardware)
  +--------------------------------------+
  | Key matrix and Chicony controller    |
  | Caps / Num / Scroll indicator LEDs   |
  +------------------+-------------------+
                     | PS/2 CLOCK + DATA
                     | (bidirectional)
                     v
           +--------------------+
           | ESP32-C3 SuperMini |
           | Scan Set 2 decode  |
           | PS/2 commands      |
           | BLE HID keyboard   |
           +----------+---------+
                      |
                      | Bluetooth Low Energy
                      v
            Windows / macOS / BLE host

  Power (experimental internal configuration):
  1S Li-ion --> IP5306 power-bank board --> 5 V USB --> ESP32 board
                                                  |
                                                  +--> 3.3 V system rail
                                                       --> HP controller
```

The PS/2 controller performs its original key scanning. The ESP32 receives transitions through its GPIO pins and constructs HID key reports. For LED synchronization, the host sends LED output reports over BLE; the ESP32 converts those states into PS/2 `0xED` commands. **The LED lights do not simply toggle locally for every operating system**: they follow the lock states actually reported by the host.

## Hardware and wiring

| Part | Purpose | Status |
| --- | --- | --- |
| HP KB-9970 PS/2 keyboard | Original mechanical assembly, controller and membrane | Retained |
| ESP32-C3 SuperMini | PS/2 host, BLE HID, serial diagnostics | Working |
| Two PS/2 signal wires + common GND | CLOCK/DATA interface | Working |
| IP5306-based power-bank module, PCB marked `D9-V1.0` | USB 5 V from a 1S Li-ion cell | Experimental |
| Blue 14500 Li-ion cell | Label: **3.7 V, 500 mAh, 1.85 Wh** | Prototype only |
| Short USB power cable | Power-bank module to ESP32 USB input | Installed |
| Original keyboard LED board | Lock-state indication | Working on Windows |

### PS/2 signal wiring

| Keyboard PS/2 signal | ESP32-C3 SuperMini |
| --- | --- |
| DATA | **GPIO4** |
| CLOCK | **GPIO3** |
| GND | **GND**, shared ground |
| Keyboard VCC | **3.3 V in this particular experimental build** |

**Electrical warning:** This particular HP keyboard is nominally a **5 V PS/2 device**, but it has been operated experimentally at **3.3 V**. The ESP32-C3 GPIOs are **not 5 V tolerant**. Do not assume a different keyboard can be connected this way; a normal 5 V PS/2 setup needs suitable level adaptation and proper pull-up verification. Never determine pinout from wire color alone.

<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/5a1e70cc-24c7-4695-84b5-74f0a7e7b6ff" />

### The original key matrix stays untouched

The original membrane and controller are still in use. No replacement matrix-scanning firmware was required.

<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/bf146ddd-a62b-48f3-a6e4-5b18b995a2f4" />

## Internal battery prototype

The battery, IP5306 board, and ESP32-C3 were fitted into existing space near the top of the HP enclosure. This keeps the standard keyboard layout and key matrix intact.

<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/61d538f5-ac41-418a-94a5-cd6401eb4525" />
<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/119dc95e-7bf6-4fe7-84c2-029c40c7121b" />
<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/a14bc7c6-4fb9-4401-b6bb-681a6f3c55d8" />



### Current power chain

```text
Blue 14500 (1S Li-ion, 3.7 V nominal, 500 mAh stated)
                 |
                 v
        IP5306 power-bank PCB
                 |
                 +--> 5 V USB output
                            |
                            v
                      ESP32-C3 USB
                            |
                            +--> regulated 3.3 V on this build
                                 --> HP keyboard/controller
```

The **battery figure is the label rating**, not an independently measured usable capacity. Battery level displayed by the current BLE constructor (`100`) is **a placeholder** and does **not** measure actual state of charge.

### IP5306 auto-shutdown and keep-alive experiment

Many power-bank controllers shut down their 5 V boost output if the attached device draws too little current. That is a problem when a Bluetooth keyboard is idle.

- The power PCB uses an **IP5306** controller (chip marking observed on the actual board).
- The current firmware experimentally generates extra CPU activity in short windows, to try to keep the power bank awake without adding a dummy load resistor or modifying the PCB.
- In the implemented test, activity starts approximately **every 18 s after the previous window**, lasting approximately **4 s**, using approximately **1.5 ms CPU-work slices** inside the main loop.
- The initial experiment **appeared to keep the power-bank output on**, but the actual load threshold, power draw, efficiency, and runtime have **not** been measured.
- **Software cannot restart a power bank after it has already disconnected power to the ESP32.** CPU activity might never meet the power bank's low-load threshold in other setups.

**This is a diagnostic workaround, not an energy-efficient power design.** A regulated, protected power solution with a genuinely always-on output would be preferable for a finished keyboard.

### Battery and charging safety

- Confirm battery chemistry, cell condition, correct polarity, protection, and the charger circuit's **actual charge current** before attempting charging.
- **The suitability of the IP5306 PCB's charge current for the small 500 mAh cell has not been established. Do not leave it charging unattended or assume it is safe to charge as assembled.**
- Never connect a bare Li-ion cell to the ESP32 **3.3 V** pin. A fully charged single Li-ion cell can be about **4.2 V**.
- Keep the battery insulated, mechanically secured, and clear of sharp screws, exposed terminals, pinched cables, and hot parts.
- A **Motorola BR50** cell was considered but **not selected**; the available unit appeared to date from 2009, so it was not considered suitable for a permanently installed rechargeable build.
- An older cell measuring **2.3 V** was discarded rather than revived.

## Firmware architecture

### PS/2 receiver

- GPIO interrupt records clock/data samples with microsecond timestamps in a bounded ring buffer.
- Main loop reconstructs PS/2 frames: start bit, eight data bits, odd parity, stop bit.
- Handles make/break sequences (`F0`), extended keys (`E0`), and the Pause sequence (`E1`).
- Keeps counters for frame faults and queue drops, with recovery when incomplete or corrupted data is detected.

### PS/2 command transmitter

- Uses open-drain-style signalling and explicit timeouts to communicate with the original controller.
- `0xEE` Echo confirms bidirectional communication.
- `0xED` updates lock LEDs; `0xFA` is the PS/2 acknowledgment used in command handling.
- LED updates are deferred while keys and HID events are pending. If a command fails, further host-to-keyboard transmission can be disabled for the current boot to protect typing.

### BLE HID layer

- Uses **HijelHID_BLEKeyboard** with **NimBLE-Arduino**.
- Preserves the BLE advertised name **`HP KB9970 V3`** for continuity with existing pairing.
- Translates PS/2 Scan Code Set 2 into USB HID keyboard usages; special consumer-media keys are handled separately.
- Uses an **8 ms minimum interval between queued HID operations** in the current default. **This is not a promise of 8 ms end-to-end Bluetooth latency.**
- Adds queue statistics, pressed-key tracking and `releaseAll()` recovery paths.

### Watchdog and diagnostics

- ESP32 task watchdog configured to **15 seconds**.
- Reports reset causes (including software, watchdog, brownout, and USB when supported by the core).
- Provides stats on PS/2 frames, HID activity, BLE readiness, free heap and lock LEDs.
- The watchdog observes loop progress; it cannot independently reset an unresponsive **keyboard controller** while that controller remains powered.

## Build and upload

Software used in development:

- Arduino IDE / Espressif **Arduino-ESP32 core 3.x**, using **ESP32C3 Dev Module**.
- **HijelHID_BLEKeyboard 0.6.x**.
- **NimBLE-Arduino 2.x**.

1. Install the appropriate ESP32 board support and libraries.
2. In Arduino IDE select **ESP32C3 Dev Module** and the correct port.
3. Enable **Tools → USB CDC On Boot → Enabled** to use the USB Serial Monitor. Rebuild/reupload after changing this option.
4. Open **`v1.ino`**, the initial firmware published in this repository, and compile/upload it. The photo/documentation package does not replace that source file.
5. Upload at **115200 baud** Serial Monitor speed, pair with **`HP KB9970 V3`** via Bluetooth, and test the keys and LEDs.
6. Back up the working `v1.ino` before trying further changes.

## Testing and platform behavior

### Windows

The converted HP was tested in Windows with working ordinary keys, special keys, numeric keypad, and **Caps Lock / Num Lock / Scroll Lock including their original indicator LEDs**. The current firmware was tested on Windows and those functions worked correctly.

### macOS

Typing and Caps Lock work over BLE. **Num Lock and Scroll Lock do not behave like their Windows counterparts**, and the Mac did not report those lock-state changes to the BLE device in the observed tests. An earlier working test build showed the same behavior on the Mac; Windows and macOS handle those lock states differently.

### Evidence from serial logs

On the HP, Num Lock and Scroll Lock produced correct PS/2 make/break codes:

```text
[KEY] MAKE 77
[KEY] BREAK 77
[KEY] MAKE 7E
[KEY] BREAK 7E
```

HijelHID also constructed the corresponding key reports:

```text
press(0x53) -> Num Lock HID usage 0x53
press(0x47) -> Scroll Lock HID usage 0x47
press(0x39) -> Caps Lock HID usage 0x39
```

In one diagnostic run the firmware reported **702 accepted PS/2 bytes**, **zero start/parity/stop errors**, **zero edge-buffer drops**, and **399 processed HID operations**. This was a **single observed session**, not a hardware performance guarantee. The HID counter records processing by firmware, not guaranteed receipt by a remote host.

### Online keyboard testing tool

[**Open the interactive keyboard tester on CodePen**](https://codepen.io/editor/PlusTer/pen/01a1b894-e483-7fb9-9cd7-3d6b39213d76)

After pairing the keyboard, open the tester, focus its test area, and try normal, modifier, navigation and numeric keypad keys. Some combinations are intercepted by the browser or OS. **A missing event in a browser does not necessarily prove an electrical keyboard failure.**

## Serial diagnostics

Monitor speed: **115200 baud**. USB CDC On Boot should be enabled when compiling for USB diagnostics.

| Serial command | Action |
| --- | --- |
| `s` | Print BLE, PS/2, HID, LED and system statistics |
| `d` | Toggle PS/2 key-event debug |
| `h` | Show available commands |
| `1` / `2` / `3` | Set HID gap to **20 / 12 / 8 ms** until next reset |
| `e` | Request PS/2 Echo |
| `l` | Request LED synchronization |
| `a` | Toggle automatic LED updates (when PS/2 TX is not blocked) |
| `b` | Restart BLE service manually |
| `r` | Restart ESP32 |
| `k` | Enable/disable CPU keep-alive experiment |
| `p` | Request a keep-alive activity window |

The HID gap returns to **8 ms** after reboot in `v1.ino`. Serial statistics are volatile and reset on reboot unless separately recorded.



### Power and components

![IP5306 power module front](<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/0f9f7cfd-5b68-45e1-8ca0-baa77ddde10a" />
)

![IP5306 power module rear](<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/62fd94be-f712-4638-9296-8a2c22c115ed" />
)

![Blue 14500 cell label 500mAh](<img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/6eb2c679-b43f-427c-a4d5-1ec7105e4031" />
)

## Compatibility, limitations, and safety

- **Confirmed keyboard:** HP KB-9970. Other PS/2 models have **not** been validated with this exact firmware and wiring.
- Different keyboard voltage, scan set, connector wiring or vendor-specific extensions may need changes.
- **USB keyboards are not PS/2 keyboards**: a USB keyboard requires a USB host implementation, not just CLOCK/DATA decoding.
- **3.3 V PS/2 operation is experimental**, even on this HP. A past freeze was observed; the cause was not established, so do not attribute it confidently to either firmware or supply voltage.
- The IP5306 keep-alive has not undergone controlled power measurements. It is possible for the bank to turn off despite CPU activity.
- The 14500's actual usable capacity, protected status and safe charge settings are **not yet confirmed**. **Avoid unattended charging and do not treat the prototype as a finished rechargeable consumer product.**
- Insulate all solder joints and exposed conductors; provide strain relief for cables and ensure closing the case cannot pinch a cell.
- **Do not short, puncture, heat, crush, or attempt to revive a deeply discharged lithium cell.**
- License has not been selected. Add a `LICENSE` file only when the author chooses one.

## Roadmap: what is still missing

This repository starts at **`v1.ino`**. Earlier development experiments are not presented as public releases. The next improvements are planned work, not features claimed to be implemented today.

### Physical recovery button

A **dedicated external reset/recovery button** is a priority. Currently, when the BLE connection or the keyboard becomes unresponsive, recovery can require unplugging power or resetting Bluetooth on the computer. A physical button connected to the ESP32-C3 **EN/RST** circuit could reboot the microcontroller without opening the case. However, resetting the ESP32 does **not** guarantee recovery from a frozen original PS/2 controller or a host-side Bluetooth pairing problem. A later revision could add a deliberate BLE re-advertise/re-pair action and, if necessary, controlled power switching for the PS/2 controller. The button is **not installed yet**.

### Better power circuit and real battery reporting

The current design repurposes a small **IP5306 power-bank PCB**, requiring an experimental CPU-load workaround to keep its 5 V output alive. The goal is to replace it with an appropriately specified **single-cell Li-ion charger, protection, and stable always-on regulator / power-path solution** designed for a low-power keyboard. The charger current must be compatible with the selected cell, and charge/discharge behaviour must be verified before routine use.

A future **fuel-gauge IC** (or properly calibrated voltage/current monitoring) should measure actual battery state and update the BLE battery-level characteristic, replacing the placeholder `100%` value. No real battery percentage is available in the current firmware.

### Other planned improvements

- Measure **actual current draw** and battery runtime in idle, active typing, with lock LEDs, and during keep-alive windows.
- Verify **safe battery charging current** and protection, with documented measurements.
- Implement and test the **real BLE battery-level reporting** described above.
- Test physical reset and BLE recovery behaviour on both Windows and macOS; improve brownout/keyboard-controller recovery.
- Refactor for multiple PS/2 keyboards using per-device profiles and tested voltage/scan-code support.
- Add a circuit diagram, build measurements and endurance-test logs alongside the existing `v1.ino`.

## Why build this?

A keyboard with a functioning key mechanism, a perfectly usable key matrix and an original controller should not become electronic waste simply because its interface is old. This project preserves the keyboard's hardware and adds a modern interface, instead of replacing what already works.

---

**Prototype:** HP KB-9970 + ESP32-C3 SuperMini + experimental 1S Li-ion/IP5306 power source.  
**Initial public firmware:** `v1.ino` (8 ms HID + watchdog + experimental CPU keep-alive).  
**Last README update:** 8 November 2026.  


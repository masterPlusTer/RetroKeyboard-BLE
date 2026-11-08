# HP KB-9970 PS/2 to Bluetooth LE Keyboard
<img width="1095" height="884" alt="image" src="https://github.com/user-attachments/assets/c6454d98-440d-4d47-b16b-7161415410f9" />

**Give a classic PS/2 keyboard a second life with an ESP32-C3 SuperMini.**

This project converts an **HP KB-9970 PS/2 keyboard** into a **Bluetooth Low Energy (BLE) HID keyboard** while keeping the keyboard's original internal controller, key matrix, and indicator LEDs. The ESP32-C3 reads the keyboard's PS/2 signals and forwards key events to a paired Bluetooth host.

The goal is to reuse working hardware rather than replace the entire key matrix or build a keyboard controller from scratch.

> **Project status:** Functional and tested on a Mac and a laptop. The current firmware is the stable **v4.5** build. The power system is still being evaluated; battery monitoring is not implemented.

## Features

- Original HP KB-9970 keyboard and controller retained.
- **PS/2 Scan Code Set 2** handling, including ordinary and extended keys.
- Bluetooth Low Energy **HID keyboard** functionality via an ESP32-C3 SuperMini.
- Bidirectional PS/2 communication between the ESP32 and the keyboard.
- Support for the keyboard's **original Caps Lock, Num Lock, and Scroll Lock LEDs**, synchronized through PS/2 commands.
- Working character keys, modifier and special keys, numeric keypad, and key-repeat behavior.
- Tested BLE HID timing configurations of **20 ms, 12 ms, and 8 ms**; **8 ms** was validated in the project.
- PS/2 diagnostic/host commands used in development include **Echo (`0xEE`)** and **Set LEDs (`0xED`)**.

## How it works

```text
 HP KB-9970
 +-----------------------------------+
 | Original key matrix               |
 | Original keyboard controller      |
 | Original Caps/Num/Scroll LEDs     |
 +----------------+------------------+
                  | PS/2: DATA + CLOCK
                  |       + power + GND
                  v
         +---------------------+
         | ESP32-C3 SuperMini  |
         | PS/2 host interface |
         | BLE HID keyboard    |
         +----------+----------+
                    |
                    | Bluetooth Low Energy
                    v
           Mac / laptop / BLE host
```

The keyboard's own controller still scans the key matrix. The ESP32-C3 acts as a **PS/2 host**, receives scan codes, translates key presses and releases into BLE HID events, and sends host-to-keyboard commands when necessary (including LED state updates).

This is **not** a direct-wire matrix conversion: the original keyboard electronics remain part of the design.

## Hardware

| Component | Role |
| --- | --- |
| HP KB-9970 PS/2 keyboard | Original keyboard, internal controller, and LEDs |
| ESP32-C3 SuperMini | PS/2 interface and BLE HID controller |
| Suitable wiring and power source | Connections and power for the two devices |
| USB power bank | Current experimental power source |

### PS/2 signal wiring (current build)


<img width="676" height="522" alt="image" src="https://github.com/user-attachments/assets/766edf7e-328a-48be-a72c-deb1237013c7" />

| PS/2 signal | ESP32-C3 SuperMini |
| --- | --- |
| DATA | **GPIO4** |
| CLOCK | **GPIO3** |
| GND | GND (common ground) |
| Keyboard VCC | See power and voltage notes below |

**Important electrical note:** The ESP32-C3 GPIOs are **3.3 V devices and are not 5 V tolerant**. Many PS/2 keyboards conventionally use 5 V; PS/2 DATA and CLOCK are open-collector/open-drain-style lines with pull-ups. **Do not assume that directly connecting an arbitrary 5 V PS/2 keyboard to the ESP32-C3 is safe.** Check the keyboard's actual supply voltage and line pull-ups, and use appropriate level adaptation when required. The HP keyboard in this build has been experimentally operated at **3.3 V**; this is not a guarantee for other keyboards.

> Before wiring another keyboard, identify its connector pinout with a reliable reference and verify it with measurements. Do not guess the pins from wire colors.

## Software

The stable **v4.5** firmware uses:

- **ESP32-C3** support in the Arduino development environment.
- **HijelHID_BLEKeyboard 0.6.x**.
- **NimBLE-Arduino 2.x**.

The firmware is responsible for receiving PS/2 scan codes, tracking pressed/released keys, sending Bluetooth HID reports, and exchanging PS/2 commands with the original controller to synchronize indicator LEDs.

### Building and uploading

1. Install the ESP32 board package in your Arduino-compatible development environment.
2. Install the compatible library versions listed above.
3. Open the **v4.5 firmware source** supplied in this repository.
4. Select the appropriate ESP32-C3 board and serial port.
5. Check the DATA and CLOCK GPIO definitions against the wiring table.
6. Compile and upload the firmware.
7. Pair the keyboard with a Bluetooth LE HID-capable computer and test the keys and indicator LEDs.

> This README intentionally does not specify an unverified firmware filename, board-menu setting, or upload command. Use the files and settings documented alongside the actual source before publishing.

## Testing and validation

The current build has been successfully tested with:

- Normal letter and number keys.
- Modifier keys and special/extended keys.
- Numeric keypad.
- Held-key repeat behavior.
- Original Caps Lock, Num Lock, and Scroll Lock indicator LEDs.
- Bluetooth keyboard operation on a Mac and a laptop.
- An **8 ms BLE HID timing setting**.

The LED functionality requires **bidirectional PS/2**, not merely listening to the keyboard's outgoing scan codes. For example, the PS/2 **Set LEDs (`0xED`)** command lets the host controller update the original keyboard's status indicators.

### Online keyboard testing tool

An interactive keyboard tester is available on CodePen to help check the converted keyboard after pairing:

**[Open the keyboard testing tool on CodePen](https://codepen.io/editor/PlusTer/pen/01a1b894-e483-7fb9-9cd7-3d6b39213d76)**

1. Connect and pair the ESP32-C3 keyboard with your computer over Bluetooth.
2. Open the tester in a browser and click inside its test area if it requires focus.
3. Press keys individually and in combinations to check that the expected events are detected.
4. Check modifier, navigation, and numeric-keypad keys, then compare the results with their behavior in ordinary applications.

**Browser limitation:** Some key combinations and special keys are intercepted by the operating system, browser, or CodePen editor. A missing response in the web tester does not necessarily mean the keyboard firmware is faulty. In particular, `Insert` may behave differently in an editor that does not support overwrite mode. Test questionable keys in another application as well.

This tool is a convenient functional check; it is not a substitute for electrical measurements or long-term Bluetooth and battery testing.

## Power supply: current state

The current setup uses a **USB power bank** for testing. A built-in battery and genuine battery-charge reporting have **not** yet been finalized.

Some power banks automatically disable their 5 V output when the connected device draws very little current. This matters for a low-power BLE keyboard: a power bank can turn itself off even when the ESP32 is working correctly. Test the chosen power bank over an extended idle period before treating it as a permanent solution.

**Do not connect a Li-ion/LiPo cell directly to a 3.3 V GPIO or power pin without checking the board's power-path requirements.** A single lithium cell can reach approximately 4.2 V when fully charged. Similarly, a power bank's 5 V USB output belongs on an appropriate 5 V/USB input, not on a 3.3 V pin.

## Compatibility and limitations

The firmware is intended as a reusable starting point for keyboards using **PS/2 Scan Code Set 2**, not just this particular HP model or its original controller.

However, **other PS/2 keyboards are not automatically plug-and-play**:

- Power-voltage requirements and line pull-ups may differ.
- Some devices use different scan-code sets or require initialization commands.
- Extended keys and vendor-specific keys may need additional mappings.
- LED synchronization and host-to-device command timing should be tested on each model.

**Confirmed functional hardware:** HP KB-9970 with the current wiring and firmware. Compatibility with other models must be verified individually.

## Future improvements

- Integrate a suitable rechargeable cell and a reliable charging/protection arrangement.
- Implement **actual battery-voltage measurement and charge reporting** rather than a placeholder battery indicator.
- Make power management suitable for long idle periods.
- Test and document additional PS/2 keyboards using the same firmware base.
- Add wiring photographs, a schematic, and precise build instructions.

## Safety

- Disconnect power before changing wiring.
- Never short a lithium cell, puncture its pouch, or charge a damaged/swollen battery.
- Verify voltages and common ground before attaching the ESP32-C3.
- Keep exposed electronics insulated before putting the keyboard into normal use.

## Project philosophy

A keyboard with a perfectly good key mechanism and working original electronics does not need to become electronic waste just because its connector is outdated. This project keeps as much of the original device as possible and adds only the interface required for modern Bluetooth use.

---

**Status:** Working prototype / stable firmware v4.5.  
**Hardware verified:** HP KB-9970 + ESP32-C3 SuperMini.  

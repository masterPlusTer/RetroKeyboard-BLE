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
- [Security considerations](#security-considerations)
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

<img width="300" height="300" alt="image" src="https://github.com/user-attachments/assets/5a1e70cc-24c7-4695-84b5-74f0a7e7b6ff" />

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

IP5306 power module front <img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/0f9f7cfd-5b68-45e1-8ca0-baa77ddde10a" />
)

IP5306 power module rear <img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/62fd94be-f712-4638-9296-8a2c22c115ed" />
 

Blue 14500 cell label 500mAh <img width="400" height="300" alt="image" src="https://github.com/user-attachments/assets/6eb2c679-b43f-427c-a4d5-1ec7105e4031" />

## Security considerations

This is an experimental DIY Bluetooth keyboard, **not a security-audited input device**. Preserving the original HP PS/2 controller does **not** independently protect keystrokes against interception or unauthorized wireless pairing. The original controller scans the keys and produces PS/2 scan codes; the ESP32-C3 firmware and BLE stack are responsible for wireless communication and its security properties.

### BLE pairing and encryption

- The project uses **HijelHID_BLEKeyboard** and **NimBLE-Arduino**. Their available security features do not by themselves prove which protections are active in a particular build or connection.
- **Pairing method, bonding behavior, encrypted-link status and authenticated/MITM-protected pairing have not been independently verified on the published `v1.ino` build.** Do not assume that seeing the keyboard connected means the link is authenticated or resistant to active attacks.
- Some BLE HID implementations use **Just Works** pairing. This method can establish an encrypted connection but does **not** provide protection against an active man-in-the-middle attacker during initial pairing. Verify the actual library settings and host behavior before making a security claim.
- Pair in a trusted environment, review the list of paired devices on the host, and remove unexpected or obsolete pairings. A future revision should investigate restricting when new pairing is accepted, authenticated pairing where supported, and documenting verified BLE security settings.

### Keystroke privacy and physical access

- The original PS/2 connection and the ESP32 wiring are **not encrypted**; someone with physical access to the exposed electronics may be able to monitor the lines. This is normal for an internal keyboard interface and separate from BLE link security.
- The firmware includes optional **Serial debugging of key activity**. Leave key-event debug disabled during normal use; do not capture or publish debug logs while typing passwords or other sensitive information. Physical USB/serial access to the ESP32 must also be considered.
- This prototype does not intentionally implement cloud transmission or remote keystroke logging. That is a design description, **not** a guarantee against vulnerabilities in firmware, dependencies or a compromised computer.
- No penetration testing, over-the-air BLE traffic validation or comprehensive source-code security audit has been completed. Use an independently trusted keyboard for especially sensitive authentication until these checks have been performed.

### Recommended validation before claiming secure operation

1. Check the **exact versions** of the BLE keyboard library, NimBLE-Arduino and ESP32 core used to build `v1.ino`.
2. Confirm the selected **pairing mode**, bonding settings and whether a connection is actually **encrypted** on Windows and macOS.
3. Test whether an **unknown host** can pair unexpectedly while the keyboard is in normal operation.
4. Confirm that debug output does not expose keystrokes in normal operation and document the procedure for clearing stored bonds.
5. Record the results in this README; until then, treat BLE security behavior as **not fully verified**.


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
**Last README update:** 9 November 2026.
**WARNING:** I used IA to vibe code and get help with the circuits and such.  
**License:** Open source.




HP KB-9970: PS/2 to Bluetooth LE, with web OTA
A solid old keyboard, a modern wireless connection, and firmware that can now be updated without opening the case.

<img width="400" alt="HP KB-9970 conversion project" src="https://github.com/user-attachments/assets/108b15ef-7ca0-43bc-b704-910b523aade9" /> <img width="400" alt="Another view of the converted HP keyboard" src="https://github.com/user-attachments/assets/0c14edef-fe18-4188-a11f-4858b8b4b23d" />
Current public firmware: V2.0.0 — November 2026. The HP KB-9970 works as a Bluetooth LE keyboard while retaining its original controller, membrane matrix and lock LEDs. A complete update to V2.0.0 through the Wi-Fi web interface has now succeeded on my laboratory keyboard. Battery operation and power management remain an evolving part of the build.

This project converts an HP KB-9970 / Chicony PS/2 keyboard into a battery-powered Bluetooth LE HID keyboard using an ESP32-C3 SuperMini. The original keyboard controller still scans the keys. The ESP32 receives PS/2 scan codes, translates them into HID reports, and sends lock-LED commands back to the original controller.

V1 established the working conversion. V2 adds a practical way to maintain it: enter update mode, connect to the keyboard's Wi-Fi, open a page, and drop in the new firmware. I can develop on an accessible laboratory keyboard before installing a proven build in the enclosed keyboard.
Contents
- Why this keyboard is worth keeping
- From V1 to V2
- What measuring power consumption taught me
- Why I am keeping the recycled power-bank board
- Batteries: the advantage of having room
- Hardware and wiring
- How the firmware works
- Build and first installation
- Updating over Wi-Fi
- Testing and platform behavior
- Serial diagnostics
- Limits and remaining work
- Development and credits
Why this keyboard is worth keeping
I like this keyboard because it is solid, full-size, and still useful. Its original membrane, controller, key layout and status LEDs are worth preserving. An old connector is a reason to adapt the interface, not a reason to discard everything behind it.

The large enclosure also has a practical advantage: there is useful space inside for the conversion electronics and a suitable battery installation. I do not have to squeeze the whole project into an enclosure designed around two tiny disposable cells. That gives this build room to evolve, provided the battery and wiring are properly secured and the power circuit matches the chosen cell.

Making it wireless removes the cable from everyday use. Adding web updates removes the need to open it for every firmware change. That combination is my idea of a super keyboard: a familiar, substantial keyboard that I can repair, adapt and keep improving.

The glowing key legends are stickers. They add personality without adding an electrically powered backlight; the original Caps, Num and Scroll indicators are separate LEDs.
From V1 to V2
Area	V1	V2.0.0
Core function	Working PS/2-to-BLE conversion	Same original controller and key matrix retained
Firmware updates	USB programming	Initial USB installation, then browser-based Wi-Fi OTA
Lock LEDs	Bidirectional PS/2 synchronization	Better state tracking, acknowledgments, retries and recovery after transient failures
HID processing	8 ms default software event gap	4 ms default software event gap, with diagnostic alternatives
Power-bank keep-alive	Periodic extra CPU activity	Extra activity limited to a 20-minute keyboard-activity window, with an exception during OTA
Maintenance	Serial commands	Serial diagnostics plus a web page with drag and drop, progress and status
Physical recovery	Power disconnection when needed	A power switch is installed in my build


V1 and V2 are the public release sequence. Names such as V6.1, V7.1 and V7.2 were laboratory iterations while developing the updater. The successful web-enabled build was consolidated as 2.0.0 so the public history remains understandable.

There is one deliberately retained legacy name: Bluetooth still advertises as HP KB9970 V3. That is the device's existing advertised name, kept for continuity with the working setup; it is not the public firmware version. The web page, /status, and startup output identify the firmware as 2.0.0.

For future releases, the intended convention is 2.0.1 for fixes, 2.1.0 for compatible additions, and a new major version for incompatible changes.
What measuring power consumption taught me
Someone on social media asked whether the original keyboard controller would consume too much power for a battery-powered conversion. It was a reasonable question: this is an older keyboard designed for a wired connection, rather than a modern battery-powered product.

After looking at the actual consumption, the important result in my build was that the ESP32 side of the conversion was the main contributor, rather than the original keyboard controller. That changed where it made sense to spend development effort. Keeping a useful old controller was not, by itself, the explanation for the battery drain.

Here, “the ESP32 side” means the conversion board and its operation as used in this assembly. It should not be read as a measurement of the bare ESP32 chip isolated from its regulator, board components and firmware behavior. Nor does this result establish the same power split for every PS/2 keyboard or ESP32 board.

The firmware work therefore focuses on avoiding unnecessary activity, recovering cleanly from errors, and preserving a good typing response. There is a real compromise: 160 MHz operation and measures intended to keep BLE responsive are performance choices, while generating extra CPU load for a power bank deliberately costs energy. A shorter HID queue interval is not, by itself, an energy-efficiency result.

The consumption conclusion above is an observation from this build. A reproducible table of current readings, measurement points and operating conditions is still to be added to this documentation. Battery runtime has not yet been established here by a controlled discharge test.
Why I am keeping the recycled power-bank board
This keyboard uses a recycled IP5306-based power-bank board. I have decided to keep that board in this particular assembly. Reusing it is part of the project, and the firmware needs to accommodate the hardware that is actually installed.

Its inconvenient feature is low-load shutdown: the board can turn off its output when the keyboard draws too little current. The IP5306 documentation describes this type of load-detection behavior, but the exact behavior of a recycled board should be checked on the actual hardware.[^ip5306]

The workaround is a CPU-load keep-alive. For short periods the ESP32 does additional work, increasing its load to discourage the power bank from switching off. In this firmware, the normal pattern is approximately 4 seconds of load activity followed by 18 seconds of rest. Work is divided into short slices in a task below the keyboard loop's priority.
The 20-minute inactivity policy
The goal is to keep the keyboard available while I am using it, including pauses to think, without keeping the power bank awake indefinitely after I walk away.
Situation	Firmware behavior
Typing, or pausing for less than 20 minutes	The normal keep-alive policy remains eligible to run
20 minutes since the last accepted keyboard activity	Stop generating the extra CPU load; stop the optional BLE warm reports as well
A new key event arrives while the electronics still have power	Renew the activity window
The power bank has already disconnected power	Use the physical power switch to restore operation
Web maintenance or firmware upload	Keep the maintenance power policy active through the update and restart


The timer follows actual accepted PS/2 key activity. LED synchronization, BLE background work and the keep-alive itself do not count as someone using the keyboard.

This is not a command to switch the battery off exactly at 20 minutes, and it is not ESP32 deep sleep. After the extra load stops, the power bank decides whether and when to shut down. If it has already removed power, an unpowered ESP32 cannot detect a key press and wake itself. My chosen recovery is simply to switch the keyboard off and on.
A new build does not have to inherit this workaround
Someone building the same conversion with a suitable supply that remains on at the keyboard's low load would not need this power-bank keep-alive. It is a consequence of my recycled power hardware, not a requirement of PS/2 or Bluetooth.

The supplied firmware retains the workaround for my board. The serial k command can disable normal CPU keep-alive for the current session, but it is not a permanent power-profile setting, and OTA maintenance has a separate override. A version intended for an always-on supply should deliberately configure that policy for its own hardware.
Batteries: the advantage of having room
The enclosure has room that makes an internal battery practical and leaves options for a suitable higher-capacity installation. That is one of the benefits of starting with a large, solid keyboard: the battery does not have to dictate the keyboard's shape.

More usable battery energy can increase runtime at the same average load, but enclosure space alone does not establish capacity, efficiency or endurance. The selected cell, charging current, protection, converter losses and actual usage all matter. This project does not yet provide a verified “runs for X days” specification.
What the earlier prototype photos show
Item	Recorded information
Cell in the earlier prototype	Blue 14500 Li-ion cell
Label	3.7 V, 500 mAh, 1.85 Wh
Usable capacity	Not independently established in this documentation
Power board	Recycled IP5306-based PCB, previously recorded as D9-V1.0
BLE battery percentage	Fixed placeholder value of 100%, not a real measurement


These are records of the pictured prototype, not a requirement to use that cell and not a claim that a larger pack has already been fitted. In particular, the small cell's label does not prove that the recycled board's charging current is appropriate for it. Cell protection, charger current, insulation and mechanical mounting must be checked for the actual battery being installed.

The current path is a single-cell battery → power-bank board → 5 V USB input on the ESP32 board. In this particular assembly, the original HP controller is powered from the board's 3.3 V rail. The battery is not connected directly to the ESP32's 3.3 V pin.
<details>
<summary>Battery, power board and internal assembly photographs</summary>
The following photographs document the earlier assembly stages. They are useful for seeing the available space and the recycled components; they are not a charging specification.
<img width="400" alt="Earlier internal assembly showing space for the conversion electronics" src="https://github.com/user-attachments/assets/61d538f5-ac41-418a-94a5-cd6401eb4525" />
<img width="400" alt="Earlier internal battery and electronics arrangement" src="https://github.com/user-attachments/assets/119dc95e-7bf6-4fe7-84c2-029c40c7121b" />
<img width="400" alt="Another view inside the HP keyboard enclosure" src="https://github.com/user-attachments/assets/a14bc7c6-4fb9-4401-b6bb-681a6f3c55d8" />
IP5306 module, front and rear:
<img width="400" alt="Front of the recycled IP5306 power module" src="https://github.com/user-attachments/assets/0f9f7cfd-5b68-45e1-8ca0-baa77ddde10a" />
<img width="400" alt="Rear of the recycled IP5306 power module" src="https://github.com/user-attachments/assets/62fd94be-f712-4638-9296-8a2c22c115ed" />
Cell label from the earlier prototype:
<img width="400" alt="Blue 14500 cell with its stated 500 mAh rating" src="https://github.com/user-attachments/assets/6eb2c679-b43f-427c-a4d5-1ec7105e4031" /></details>Hardware and wiring
Part	Role
HP KB-9970 PS/2 keyboard	Original enclosure, key matrix, controller and status LEDs
ESP32-C3 SuperMini	PS/2 host, BLE HID device and Wi-Fi web updater
CLOCK, DATA and common ground	Bidirectional PS/2 connection
Recycled IP5306 power board	Battery charging/boost circuitry used in this assembly
Single-cell Li-ion battery	Internal energy storage; suitability depends on the actual cell and board
Physical power switch	Manual power cycling


Signal connections
Keyboard signal	ESP32-C3 connection
DATA	GPIO4
CLOCK	GPIO3
GND	Common GND
Keyboard supply	3.3 V in this experimental build


Electrical compatibility matters. This HP keyboard is nominally a 5 V PS/2 device, and operating its controller at 3.3 V is a choice tested in this particular build. ESP32-C3 GPIOs must not be treated as 5 V-tolerant inputs. A keyboard powered at 5 V needs appropriate bidirectional level handling and verified pull-ups before connecting CLOCK and DATA. Check the actual pinout rather than relying on wire colors.[^esp32]
<img width="300" alt="PS/2 wiring used in the conversion" src="https://github.com/user-attachments/assets/5a1e70cc-24c7-4695-84b5-74f0a7e7b6ff" />The original matrix stays intact
The HP controller performs the matrix scanning it was designed to do. The ESP32 works at the PS/2 interface, so this conversion does not require tracing and reimplementing the complete membrane matrix.
<img width="400" alt="Original HP keyboard membrane matrix retained in the build" src="https://github.com/user-attachments/assets/bf146ddd-a62b-48f3-a6e4-5b18b995a2f4" />
The firmware targets PS/2 Scan Code Set 2. Other PS/2 keyboards may be adaptable, but their supply levels, scan codes, special keys and LED behavior need individual checks. A USB keyboard requires a USB host implementation; this CLOCK/DATA interface does not provide one.
How the firmware works
Two operating modes
Normal operation uses Bluetooth with Wi-Fi off. Maintenance uses Wi-Fi without starting Bluetooth. Entering maintenance saves a request and restarts the ESP32 into that mode; it does not try to keep typing over BLE during an upload.
```mermaid
flowchart TD
    A["Power on or restart"] --> B{"Enter web maintenance?"}
    B -->|"No"| C["Bluetooth keyboard"]
    C -->|"Shortcut or serial o"| D["Save request and restart"]
    D --> A
    B -->|"Yes"| E["Wi-Fi web updater"]
    E -->|"Successful upload, exit, or timeout"| A
```
The first installation of the web-enabled firmware can open maintenance automatically once. Later entries normally come from the shortcut or serial command. The internal NVS marker named web72 is retained for compatibility with the laboratory firmware; it is not the current version number.
PS/2 reception and HID reports
Clock interrupts capture DATA samples and timestamps into a bounded queue. The main loop validates frames, decodes Scan Code Set 2, and converts make/break sequences into keyboard or supported consumer-media HID events.

The receiver handles extended keys and special sequences. Queue limits, frame-error counters, pressed-key tracking and release/recovery paths are intended to stop a bad frame or stale event from silently leaving a key held.

V2 requests a 160 MHz CPU clock, within the ESP32-C3's specified operating range.[^esp32] The default software gap between queued HID operations is 4 ms. The BLE library is asked for a fast connection interval; the host ultimately negotiates the actual radio timing.[^hijel]

A 4 ms software gap is not a measured 4 ms end-to-end key latency. These settings and the queue changes target responsiveness; an instrumented latency benchmark remains separate work.
Two different kinds of extra activity
The CPU keep-alive for the power bank is separate from the optional BLE warm reports. While the keyboard is still inside its activity window, the latter can send an empty HID report about every 500 ms when no keys are held and there is no pending key traffic. This is intended to reduce wake-up delay after pauses without generating characters or renewing the user's inactivity timer.

Both mechanisms have a cost. They are specific firmware tradeoffs, not evidence of the lowest possible power consumption. Serial k controls the normal CPU workaround; z controls the empty BLE reports.
Original lock LEDs, with recovery
The computer reports its lock states through BLE. The firmware tracks the desired LED state, the command being sent, and the state actually acknowledged by the PS/2 controller.

It sends PS/2 0xED and the LED mask with acknowledgment handling, supports 0xFE resend responses, and uses 0xEE Echo with delayed retries to recover after errors. A transient command failure no longer means abandoning automatic LED synchronization for the rest of that boot.

The LEDs follow the host's reported state. The firmware does not simply flip Caps Lock locally whenever that key is pressed, and it cannot invent a Num Lock or Scroll Lock state that the host never reports.
Watchdog and diagnostics
Normal operation configures a 15-second task watchdog; maintenance uses a longer 60-second watchdog. Reset causes, queue activity, LED state, free memory and other diagnostics are available over USB serial. The keyboard starts without waiting for a Serial Monitor connection.

A watchdog reset of the ESP32 is not necessarily a reset of the original HP controller. The physical power switch remains useful when the entire assembly needs a fresh start.
Build and first installation
Development configuration
Setting	Configuration used for web OTA development
IDE	Arduino IDE
Board	ESP32C3 Dev Module
Espressif Arduino-ESP32 core	3.3.10 in the recorded build configuration
CPU	160 MHz
Flash	4 MB on the development board
Partition scheme	OTA-capable default scheme used by this build
USB CDC On Boot	Enabled for native USB diagnostics
Serial Monitor speed	115200 baud
BLE libraries	HijelHID_BLEKeyboard and NimBLE-Arduino


Keep the board and library configuration that compiled the working build. Exact BLE-library patch versions should be recorded with future releases; they are not fully pinned by this README. Upstream library documentation describes available APIs, but this firmware's own settings determine the behavior of this build.[^hijel][^nimble]

The current source is HP_KB9970_V2_0_0.ino. Put it in the matching Arduino sketch folder HP_KB9970_V2_0_0. Keep v1.ino as the original public version in the project history.

For a board that does not yet run this web-enabled firmware:
1. Install the ESP32 board support and the required BLE libraries.
2. Select the correct ESP32-C3 board, flash size and OTA-capable partition scheme.
3. Open the V2 sketch, compile it, and upload by USB.
4. The first installation of the web firmware may open maintenance automatically. Use the web page's return button or wait for the maintenance window to expire to return to Bluetooth.
5. Pair with HP KB9970 V3 and check keys, LEDs and the update shortcut.
For the enclosed keyboard, the intended procedure is to install the complete version already tested on the laboratory keyboard. There is no need to first install a separate older “OTA program”: the keyboard application and the web updater are included in the same firmware.
Preserve the OTA layout
The recorded partition table has two application slots, 1,310,720 bytes each, plus OTA metadata. A new application must fit in its destination slot. The compiled .bin size, not the .ino source-file size, is what matters.

Application OTA writes the other application slot and selects it after validation. The page in this project updates the application only; it does not change the partition table or bootloader.[^ota] Keep the existing layout when compiling subsequent updates. Do not switch to a “No OTA” layout just to make a larger binary fit.
Updating over Wi-Fi
Connection details
Item	Value
Enter maintenance	Hold Ctrl + Alt + F12 for 3 seconds, then release all keys
USB serial alternative	Send o
Wi-Fi network	HP-KB9970-WEB
Wi-Fi password	teclado1
Update page	http://192.168.4.1/
Upload / return-button password	teclado
Status endpoint	http://192.168.4.1/status


The network comes from the keyboard's own access point. An internet connection, home router and USB connection are not required for the browser upload.[^wifi] Compile and export the file before switching the computer to the keyboard's Wi-Fi.
Upload procedure
1. Open the new sketch with the same board and partition settings, then use Sketch → Export Compiled Binary in Arduino IDE.
2. Choose the application binary. For this release it is HP_KB9970_V2_0_0.ino.bin.
3. Enter maintenance with the shortcut, then connect the computer or phone to HP-KB9970-WEB.
4. Open http://192.168.4.1/. Opening the page does not require a username or password.
5. Drag the application .bin into the page, or click the file area to select it. Enter teclado and press Actualizar teclado.
6. Keep power connected while the transfer and validation complete. After a successful update, the keyboard restarts into the new firmware.
7. Confirm that Bluetooth reconnects and normal typing works. Then enter maintenance again and check that the page or /status reports the new version.
Do not upload the source .ino, a ZIP, .merged.bin, .bootloader.bin, .partitions.bin, or boot_app0.bin. The updater expects a single ESP32-C3 application image.

The page checks file size against the available application slot. The receiver checks the expected image structure, target chip, transfer length and update result before committing it. These checks catch several common upload mistakes; they do not establish that a new application is bug-free or provide a signed-firmware system.
What happens during maintenance
Bluetooth disconnects while the keyboard runs its Wi-Fi-only updater. This is expected. Use another input device if you need to type on the computer during maintenance.

The page has a 15-minute window to begin an update. Visiting or refreshing it does not keep extending that deadline. Once a transfer starts, it has separate transfer limits. If no upload is started, the timeout returns the device to Bluetooth. The Volver al teclado button, using the same teclado password, also leaves maintenance.

The firmware retains the 8.5 dBm requested Wi-Fi transmit-power limit that made the access point visible on my SuperMini during development. This is a working setting for this board, not a universal diagnosis for every ESP32-C3 Wi-Fi problem.

Every future firmware image must retain the OTA implementation if wireless updating is to remain available. Installing an otherwise valid application without the updater can remove the ability to update it this way.
If the page does not open
Check that the computer is actually connected to HP-KB9970-WEB, that maintenance has not timed out, and that the address starts with http://. This network does not provide internet access.

On macOS versions with Local Network privacy controls, check the browser's permission under System Settings → Privacy & Security → Local Network. Trying the same address in another browser can help distinguish a browser issue from a connection issue.[^apple]
Testing and platform behavior
What has been demonstrated
Observation	Scope
Ordinary keys, supported special keys and numeric keypad work over BLE	Observed on the converted HP keyboard
Original Caps / Num / Scroll LEDs respond on Windows	Observed during project testing
Typing and Caps Lock work on macOS	Observed during project testing
The web page opens and shows firmware status	Observed on the laboratory keyboard
Complete wireless update to V2.0.0	Successfully performed on the laboratory keyboard
ESP32 side identified as the main consumption contributor	Author's observations from this assembly; detailed measurement table still pending


This confirms the update workflow on the laboratory build. The next checks are longer typing sessions, reconnection and LED recovery.
Host-dependent lock keys
In the macOS tests, Num Lock and Scroll Lock did not behave like their Windows counterparts, and the corresponding LED-state changes were not reported in the same way. The firmware follows the states supplied by the host; it cannot make every operating system treat those keys identically.

Earlier diagnostic logs showed correct PS/2 make/break codes for Num Lock and Scroll Lock, and the corresponding HID usages were generated. Those historical logs helped separate keyboard decoding from host behavior; they are not a new V2 performance benchmark.
Keyboard tester
Open the keyboard tester used during development on CodePen

After pairing, focus the test area and check normal keys, modifiers, navigation and the keypad. Browsers and operating systems intercept some shortcuts, so a missing browser event alone does not prove an electrical fault.
Serial diagnostics
Use 115200 baud and enable USB CDC On Boot when compiling for native USB diagnostics. Serial is optional during normal use and browser OTA.
Command	Action
s	Print statistics
h	Show help
d	Toggle key-event debug output
1 / 2 / 3 / 4 / 5	Set the software HID gap to 20 / 12 / 8 / 4 / 2 ms
e	Request PS/2 Echo
l	Request LED synchronization
a	Toggle automatic LED synchronization
b	Restart BLE when no PS/2 command/recovery or OTA transition is pending
r	Restart the ESP32 in normal mode
k	Toggle normal-mode CPU keep-alive
p	Request a CPU-load window while the activity period is still open; also re-enable normal keep-alive
z	Toggle empty BLE warm reports
o	Request web maintenance
x	Cancel an OTA-entry request or leave maintenance when appropriate


Diagnostic settings are temporary. On restart, V2 returns to its defaults, including the 4 ms HID gap. Maintenance only accepts the relevant commands; it does not permit an ordinary BLE restart command in the middle of an upload.

Leave key-event debug off during everyday use: those logs can contain information about what is being typed.
Limits and remaining work
Power and battery reporting
The current firmware does not measure battery capacity, current or state of charge. A BLE display of 100% is a placeholder. Adding a suitable fuel gauge or validated measurement circuit remains future work.

The next useful power record is a set of repeatable measurements: original controller, ESP32 board, typing, idle, lock LEDs, keep-alive windows and OTA, with the voltage and measurement point specified. A full discharge test would then establish usable runtime for a named battery and usage pattern.

I am keeping the recycled IP5306 board in this keyboard. A different always-on supply remains an option for other builders, rather than a promised hardware replacement in my own build.
Recovery and compatibility
The HP KB-9970 is the confirmed keyboard. Other models, supply arrangements and special-key implementations are not automatically covered. A prior freeze was observed during development without an established cause; neither undervoltage nor a particular software bug should be declared responsible without evidence.

The physical power switch can restart the whole assembly. It is not the same thing as a separately implemented ESP32 reset button, a PS/2 controller power switch, or a wake-on-key circuit after the power bank shuts down.

Keep the known-working source and compiled application binary. OTA validation rejects several incomplete or incompatible uploads, but a valid image can still contain a software error that prevents subsequent wireless recovery. USB remains the recovery path for that case.
Wireless access
The firmware explicitly selects Just Works BLE pairing. That choice favors simple pairing and should not be described as a verified authenticated-pairing security design. No independent security audit is claimed.[^hijel]

The maintenance page uses local HTTP and the simple default passwords listed above. Anyone with those defaults and access to the maintenance network can authorize an upload. Change the credentials in a build intended for an environment where other people should not have that access. The image checks are not cryptographic firmware signatures.
What I want to improve next
- Publish the current measurements with their conditions and a controlled runtime test.
- Add real battery reporting instead of the fixed percentage.
- Continue testing LED recovery, reconnection and long typing sessions on the laboratory keyboard.
- Record exact dependency versions alongside each release.
- Keep the enclosed keyboard on a version already exercised on the laboratory build.
Development and credits
This is an iterative hardware-reuse project. I used AI assistance to develop and review firmware and to discuss circuit ideas, then tested changes on the actual hardware. The working result comes from that build-and-test process; AI-generated suggestions are not a substitute for measurements or hardware validation.

The BLE implementation uses HijelHID_BLEKeyboard and NimBLE-Arduino, on the Espressif Arduino-ESP32 core.

Project license: pending selection. Dependency licenses remain those provided by their respective projects.
Current public firmware: V2.0.0
Original public firmware: v1.ino
README updated: 21 November 2026
Technical references
The build observations above come from this project's hardware tests, firmware and development history. The references below describe the underlying components and APIs; they do not independently verify this keyboard's current draw or battery runtime.

[^esp32]: Espressif — ESP32-C3 datasheet. CPU capabilities and electrical operating limits.
[^ip5306]: Injoinic — IP5306 datasheet, hosted by LCSC. Power-bank controller and low-load behavior; check the actual board implementation.
[^hijel]: HijelHID_BLEKeyboard — upstream project. HID interface, connection settings and pairing options. Upstream revisions may differ from the version installed for a particular build.
[^nimble]: NimBLE-Arduino — upstream project.
[^ota]: Espressif — ESP32-C3 OTA documentation. Application slots and OTA metadata.
[^wifi]: Espressif — Arduino-ESP32 Wi-Fi API. Access-point mode.
[^apple]: Apple — Control access to your local network on Mac.

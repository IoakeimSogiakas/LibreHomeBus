
## What’s New in Alpha

The project moves from Pre-Alpha to Alpha. The main change is that it now runs on a real RS-485 bus instead of a single board doing UART testing from Python.

Two CH32V003 nodes on a physical RS-485 bus, talking to a PC through a USB-RS485 dongle.
Discovery works on real hardware. Both nodes received unique addresses from the script.
The binary search is more forgiving of the timing on a real bus. A “no answer” now means three tries with no reply, and the result is checked afterwards: the lowest UID must answer and the number just below it must not. If the check fails, the search runs again.
Fixed CRC failures on the slave. Debug prints inside the frame polling loop were slow enough to corrupt frames on real RS-485. Per-frame logging is now behind a DEBUG_BUS switch in config.h, off by default.
Tested a switch (FID 1) driving a dimmer (FID 3) through both Home Assistant automations and ESPHome automations. The ESPHome version responds noticeably faster. The cause isn’t determined. The test setup has several hops (dongle, Windows, WSL, ESPHome host build, Home Assistant), and the extra latency might also be inherent to the Home Assistant path. This will be revisited with a real ESP master.
Scripts are being cut down. Only Descovery.py and bus_test.py will remain. bus_test.py is for bench testing nodes by hand.

## Why LibreHomeBus?

LibreHomeBus follows the architecture of professional centralized wired smart home systems like Loxone, but it is open source and costs far less.

Commercial setups can run to tens of thousands of dollars and often use proprietary bus encryption. In a home, fieldbus encryption adds little. To attack the bus, someone has to get into the house and open a wall. LibreHomeBus skips it in favor of simplicity, speed and low cost.


## 48V DC Room Topology 

A typical smart bulb contains its own AC-to-DC converter, so a house with fifty bulbs has fifty small mains power supplies. In cheap bulbs that converter sits right next to the LEDs in a sealed enclosure. The heat from the LEDs and the converter cooks its components, especially the electrolytic capacitors, and that is one of the most common ways LED bulbs fail.

LibreHomeBus does the AC-to-DC conversion once per room (or once per house) with a single power supply. That supply feeds a DC rail that runs next to the RS-485 data bus. Each node steps the rail down locally with a buck converter for the microcontroller and transceiver, and uses a dedicated LED driver for the light. The drivers sit apart from the LEDs, so the electronics stay cooler and last longer. Separating the driver from the LEDs is also common in good commercial lighting. This system just applies it throughout.

The rail is 48V for now, and the first prototype PCB will use it. It may change to 52V later. Earlier versions said 54V, and the power supply side is still untested, so treat the voltage as provisional.

A CH32V003 plus a proper LED driver makes a fixture with well-spaced, properly rated parts. The preliminary BOM for a custom SPI-based tunable white light is about €6.

*(Note: Schematics, BOMs, and Gerber files for the custom light fixtures and wall switches will be published in this repository as the hardware phase of the project matures.)*

---

# Project Status: Alpha

The firmware and discovery script work on a physical RS-485 bus with two nodes. This is still a prototype, and cleanup and refactoring are ongoing.

### Important Notes
* **Test setup:** Development is done with the ESPHome host build running in WSL, with the USB-RS485 dongle passed through with usbipd. A real ESP master hasn’t been tested yet.

* **Bus size:** Discovery has only been tested with two nodes. Larger multi-node buses are untested.

* **Scripts:** Two scripts remain in tools/. Descovery.py finds nodes, assigns addresses and writes a basic hub.yaml for ESPHome. bus_test.py is a menu for bench testing (read and write registers, on/off checks, identify LED test, watch a register, forget a node by UID). They work and can be used as they are, but they’re clunky. Settings like the serial port and ESPHome target are constants at the top of each file, and there is no command-line interface. A proper commissioning flow and a graphical interface are planned for the future.
* **Changing addresses:** There’s no separate script for it. Use the “forget a node by UID” option in bus_test.py to clear a node’s saved address, then run discovery again. Discovery picks the lowest free address, and you can’t choose one yourself yet. (I will probably add a way in the bus_test skript if I find a need for that)
* **ESPHome config generation:** Basic generation works for FID 1, 2, 3, 5 and 6. FID 4 has no YAML mapping yet and is skipped. Node identification and automation coupling are planned. See Future Plans.
* **PCB testing:** Not started. Everything so far runs on dev boards and breadboards. See Future Plans.


---

## Flavor Status

| FID | Personality | Registers Used | Interface | Status |
|:---:|:---|:---|:---|:---:|
| 1 | Toggle Switch Input | Reg 1 (State, read-only), Reg 2 (Identify LED) | Digital input, debounced with internal pull-up, PCB LED | RS-485 bus tested, LED bench tested |
| 2 | Relay / On-Off Light | Reg 1 (On/Off) | Push-pull digital output | RS-485 bus tested |
| 3 | Single-Channel Dimmer | Reg 1 (On/Off), Reg 2 (0..1000) | TIM1 CH1 PWM at 48 kHz | RS-485 bus tested |
| 4 | Tunable White (SPI) | Pending | Dedicated SPI driver IC | Pending |
| 5 | Tunable White (Dual PWM) | Reg 1 (On/Off), Reg 2 (Cold), Reg 3 (Warm) | TIM1 CH1 + CH3 PWM at 1 kHz | Bench tested |
| 6 | Wall Plug | Reg 1 (On/Off), Reg 2 (Identify LED) | Relay output, local toggle switch (no register), PCB LED | RS-485 bus tested |

Register 0 always holds the FID and is read-only. The identify LED register is only written by the commissioning script, and the node turns the LED off by itself after 30 s.

FID 4 is planned for bench testing next.

---

## Architecture & Hardware

```text
[ Home Assistant ]
       |
       |  Native API (Wi-Fi or Ethernet)
       v
[ ESPHome Master ] (ESP32 / ESP8266)
       |
       |  Modbus RTU (115200 baud, UART + DE/RE direction pin)
       v
======= RS-485 Bus ==============================================
       |                         |                         |
 [ CH32V003 Node ]         [ CH32V003 Node ]         [ CH32V003 Node ]
 Flavor 1 (Switch)         Flavor 2 (Relay)          Flavor 5 (TW Light)
==================================================================

```
---

### Why ESPHome as the Master?

Home Assistant's native Modbus integration relies on polling loops that are too sluggish for wall switches. Running the bus master on an ESP via ESPHome allows continuous, low-latency polling directly over the hardware UART. Input changes trigger immediate state pushes to Home Assistant over the native API.

### Hardware Notes
* **Transceivers & Voltage:** The system is currently designed and bench-tested around a **5V logic level** using the SN65HVD3082E RS-485 transceiver. Any standard 3.3V or 5V transceiver (MAX485, clones, isolated modules) is compatible, provided the logic levels match your supply rail.
* **Node Connections:** The CH32V003 slave firmware uses `PD5` for TX, `PD6` for RX, and `PD4` to control the transceiver's DE and /RE lines tied together.
* **Master Connections & Level Shifting:** The ESP requires three pins wired to its transceiver: UART TX, UART RX, and a GPIO driving DE and /RE. Since **all ESP32 and ESP8266 microcontrollers use 3.3V logic** (including ethernet gateways like the WT32-ETH01), if you are pairing the ESP with a 5V transceiver, you must step down the transceiver's 5V RX output to the ESP's 3.3V RX input using a simple voltage divider (e.g., a 2kΩ and 1kΩ resistor pair).

---

## Commissioning Protocol

New or unassigned nodes run with address 0 (`DEFAULT_ID = 0`). The discovery script uses a binary search across the 64-bit factory UID space, following a model similar to DALI commissioning:

1. **Compare (`0x41`):** The master broadcasts a UID threshold. Any unassigned node whose factory UID is less than or equal to that number answers with a single confirmation byte (`0x55`).

2. **Binary Search:** By halving the 64-bit UID search window based on whether anyone answers, the master isolates the lowest UID on the bus in roughly 64 queries.

3. **Assign Address (`0x42`):** Once a single chip is isolated, the master sends a broadcast command with that specific UID and its new address (1–247).

4. **Flash Storage:** The chip writes the new address into its last 64-byte flash page (`0x08003FC0`), so the address survives reboots and firmware reflashes.

5. **Clear / Reset (`0x43`):** Broadcast command that tells a specific UID to erase its saved address and revert to unassigned mode.

6. **Identification LED (switch and plug only):** The switch (FID 1) and plug (FID 6) flavors have an LED on `PC2`, controlled by register 2. Once a node has an address, the master can turn the LED on with a normal write (function `0x06`) so you can see which physical device got which address. It turns itself off after 30 seconds if the master forgets. The current discovery script doesn't use it yet, it's there for a later version.

---

## Future Plans
* **Node identification:** Before naming a node, the script asks which physical device it is. It uses the identify register on flavors that have one. On lights it can turn the light on.
* **Automation coupling:** Link inputs to outputs from the script, for example a switch to a specific light.
* **Better ESPHome YAML generation:** Read the discovered flavors, map the registers, and write a ready-to-flash config.
* **Caching:** The script will cache what it learns (for example the addresses, UIDs and flavors of nodes it has already found) so later runs are faster and don’t repeat the full discovery.
* **Real ESP master:** Move off the WSL host build and recheck latency, including the Home Assistant path. (this will probally happen in the next week)
* **PCB testing:** Prototype boards on the 48V rail. Planned for a few months from now.

## Licensing

This project is dual-licensed to cover both hardware designs and software/firmware components:

* **Firmware & Software Tools:** Licensed under [GNU General Public License v3.0 (GPL-3.0)](LICENSE).
* **Hardware (KiCad Schematics, PCB Layouts, & Libraries):** Licensed under [CERN-OHL-S v2 (Strong Reciprocal)](LICENSE-HARDWARE).
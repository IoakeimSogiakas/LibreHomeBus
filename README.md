## Why LibreHomeBus?

LibreHomeBus is designed to mimic the architecture of professional, centralized wired smart home systems like Loxone, but open-source and at a fraction of the cost. 

Commercial setups can easily cost tens of thousands of dollars and often feature proprietary bus encryption. In a residential physical environment, fieldbus encryption is largely unnecessary—an attacker would literally need to break into your house and tear open a wall to splice into the wiring. LibreHomeBus trades that over-engineering for simplicity, speed, and extreme cost-efficiency.

### The 54V DC Room Topology
Traditional smart bulbs cram a tiny, inefficient AC-to-DC converter inside the bulb base. The resulting heat cooks the components and drastically reduces the lifespan of the electronics. 

LibreHomeBus eliminates this by distributing a single, robust 54V DC power rail alongside the RS-485 data bus for each room. One industrial AC-to-DC power supply runs an entire room's lighting and switches. Each local node then uses a simple, highly efficient buck converter to step down the 54V line to power the microcontroller and transceiver logic.

This centralized power architecture allows the end nodes to be incredibly simple and cheap. By pairing ultra-low-cost CH32V003 microcontrollers with high-quality LED drivers, you can build smart, highly efficient, and long-lasting fixtures where components are spaced out and spec'd to survive. Preliminary BOM costs for a custom SPI-based tunable white light are approximately €6. 

*(Note: Schematics, BOMs, and Gerber files for the custom light fixtures and wall switches will be published in this repository as the hardware phase of the project matures.)*

---

## Project Status: Pre-Alpha

This repository represents an active engineering prototype. The codebase is functional on bench hardware, but structural refactoring and code cleanup remain in progress.

### Important Notes
- **Development Scripts:** The Python utilities located in `tools/` are active bench-testing and debugging scripts. They do not represent the finalized 1.0 architecture (which will provide a complete commissioning workflow and graphical interface).
- **Bus Commissioning Verification:** Full end-to-end verification of automatic binary discovery across large, multi-node buses is currently ongoing.
- **Manual ID Modification:** Address modification logic exists in raw form (`tools/ChangingID.py`), but documentation and standardized workflows for it are deferred to a later release.
- **Pending ESPHome Integration:** Automated generation of the master ESPHome YAML configuration is in development. The planned toolchain will inspect discovered bus flavors, map register assignments, and emit ready-to-flash YAML configurations.


---

## Flavor Status

| FID | Personality | Registers Used | Interface | Status |
|:---:|:---|:---|:---|:---:|
| 1 | Toggle Switch Input | Reg 1 (State, read-only), Reg 2 (Identify LED) | Digital input, debounced with internal pull-up, PCB LED | Switch bench tested, LED untested |
| 2 | Relay / On-Off Light | Reg 1 (On/Off) | Push-pull digital output | Bench tested |
| 3 | Single-Channel Dimmer | Reg 1 (On/Off), Reg 2 (0..1000) | TIM1 CH1 PWM at 48 kHz | Bench tested |
| 4 | Tunable White (SPI) | Pending | Dedicated SPI driver IC | Pending |
| 5 | Tunable White (Dual PWM) | Reg 1 (On/Off), Reg 2 (Cold), Reg 3 (Warm) | TIM1 CH1 + CH3 PWM at 1 kHz | Bench tested |
| 6 | Wall Plug | Reg 1 (On/Off), Reg 2 (Identify LED) | Relay output, local toggle switch (no register), PCB LED | Untested |

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

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**. Refer to the `LICENSE` file for full terms and conditions.

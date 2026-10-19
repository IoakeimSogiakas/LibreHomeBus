import time
import serial

PORT           = "COM5"
TARGET         = "host" #"wt32"
HUB_NAME       = "hub-1"
YAML_FILE      = "hub.yaml"
HOST_UART_PORT = "/dev/ttyACM0"         # the same dongle as ESPHome sees it, host target only
SCAN_TIMEOUT   = 0.03
COLD_K, WARM_K = 6500, 2700

# Nodes start answering about 2 ms after a request.
SEND_WAIT      = "30ms"
# Keep above 0. Nodes close a frame after 1.75 ms of silence.
TURNAROUND     = "2ms"

MAX_UID = 2**64 - 1
s = None


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def frame(hexstr: str, bad: bool = False) -> bytes:
    body = bytes.fromhex(hexstr)
    crc = crc16(body)
    out = body + bytes([crc & 0xFF, crc >> 8])
    if bad:
        out = out[:-1] + bytes([out[-1] ^ 0xFF])
    return out


def anyone_answers(threshold: int) -> bool:
    s.reset_input_buffer()
    s.write(frame("00 41 " + threshold.to_bytes(8, "big").hex(" ")))
    s.flush()
    return len(s.read(1)) > 0


def find_lowest_unassigned():
    if not anyone_answers(MAX_UID):
        return None
    low, high = 0, MAX_UID
    while low < high:
        mid = (low + high) // 2
        if anyone_answers(mid):
            high = mid
        else:
            low = mid + 1
        time.sleep(0.005)
    return low


def assign(uid: int, addr: int):
    s.write(frame("00 42 " + uid.to_bytes(8, "big").hex(" ") + f" {addr:02X}"))
    s.flush()
    time.sleep(0.05)


def probe(addr: int):
    """Returns (answered, fid). fid is None if the reply was garbled."""
    s.reset_input_buffer()
    s.write(frame(f"{addr:02X} 03 00 00 00 01"))
    s.flush()
    r = s.read(7)
    if not r:
        return False, None
    if len(r) != 7 or crc16(r) != 0 or r[0] != addr or r[1] != 0x03:
        return True, None
    return True, (r[3] << 8) | r[4]


def scan_assigned() -> dict:
    """Returns {address: fid}. fid is None if the reply was garbled."""
    found = {}
    old = s.timeout
    s.timeout = SCAN_TIMEOUT
    for addr in range(1, 248):
        answered, fid = probe(addr)
        if answered:
            found[addr] = fid
            if fid is None:
                print(f"Address {addr}: garbled reply, address collision possible")
    s.timeout = old
    return found


def next_free(devices: dict):
    return next((a for a in range(1, 248) if a not in devices), None)


# The wt32 pins and Ethernet settings are placeholders. Check them against the
# WT32-ETH01 pinout.
HEADERS = {
"wt32": """esphome:
  name: {name}

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:

api:

ota:
  - platform: esphome

ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO0_IN
  phy_addr: 1
  power_pin: GPIO16

uart:
  id: uart_bus
  tx_pin: GPIO17
  rx_pin: GPIO32
  flow_control_pin: GPIO33
  baud_rate: 115200
  rx_buffer_size: 256
  rx_timeout: 15

modbus:
  id: modbus_bus
  uart_id: uart_bus
  send_wait_time: {send_wait}
  turnaround_time: {turnaround}
""",
"host": """esphome:
  name: {name}

host:
  mac_address: "02:A1:B2:C3:D4:E5"

logger:

api:

uart:
  id: uart_bus
  port: {port}
  baud_rate: 115200
  rx_buffer_size: 256

modbus:
  id: modbus_bus
  uart_id: uart_bus
  send_wait_time: {send_wait}
  turnaround_time: {turnaround}
""",
}

# FID 0 and 4 are left out on purpose.
LABELS = {1: "Switch", 2: "Light", 3: "Dimmer", 5: "Tunable light", 6: "Plug"}


def output_block(n, key, reg, multiply=None):
    text = (f"  - platform: modbus_controller\n"
            f"    modbus_controller_id: node_{n}\n"
            f"    id: node_{n}_{key}\n"
            f"    address: {reg}\n"
            f"    register_type: holding\n"
            f"    value_type: U_WORD\n")
    if multiply:
        text += f"    multiply: {multiply}\n"
    return text


def light_block(n, name, platform, lines):
    return (f"  - platform: {platform}\n"
            f"    name: \"{name}\"\n"
            + "".join(f"    {l}\n" for l in lines) +
            f"    default_transition_length: 0s\n"
            f"    on_turn_on:\n"
            f"      - output.turn_on: node_{n}_power\n"
            f"    on_turn_off:\n"
            f"      - output.turn_off: node_{n}_power\n")


def write_yaml(devices: dict):
    sec = {"modbus_controller": [], "binary_sensor": [], "switch": [], "output": [], "light": []}

    for n in sorted(devices):
        fid = devices[n]
        if fid not in LABELS:
            print(f"Address {n}: FID {fid} has no YAML mapping, skipping")
            continue

        name = f"{LABELS[fid]} {n}"

        if fid == 1:
            poll = ("    continuous: true\n"
                    "    update_interval: 1s\n"
                    "    offline_skip_updates: 9\n")
        elif fid in (2, 6):
            poll = ("    update_interval: 1s\n"
                    "    offline_skip_updates: 9\n")
        else:
            poll = ""    # output-only nodes have nothing to read

        sec["modbus_controller"].append(
            f"  - id: node_{n}\n"
            f"    address: {n}\n"
            f"    modbus_id: modbus_bus\n" + poll)

        if fid == 1:
            sec["binary_sensor"].append(
                f"  - platform: modbus_controller\n"
                f"    modbus_controller_id: node_{n}\n"
                f"    name: \"{name}\"\n"
                f"    register_type: holding\n"
                f"    address: 1\n")
        elif fid in (2, 6):
            sec["switch"].append(
                f"  - platform: modbus_controller\n"
                f"    modbus_controller_id: node_{n}\n"
                f"    name: \"{name}\"\n"
                f"    register_type: holding\n"
                f"    address: 1\n"
                f"    bitmask: 1\n")
        elif fid == 3:
            sec["output"] += [output_block(n, "power", 1),
                              output_block(n, "brightness", 2, 1000)]
            sec["light"].append(light_block(n, name, "monochromatic",
                                            [f"output: node_{n}_brightness"]))
        elif fid == 5:
            sec["output"] += [output_block(n, "power", 1),
                              output_block(n, "cold", 2, 1000),
                              output_block(n, "warm", 3, 1000)]
            sec["light"].append(light_block(n, name, "cwww", [
                f"cold_white: node_{n}_cold",
                f"warm_white: node_{n}_warm",
                f"cold_white_color_temperature: {COLD_K} K",
                f"warm_white_color_temperature: {WARM_K} K",
                "constant_brightness: true"]))

    text = HEADERS[TARGET].format(name=HUB_NAME, port=HOST_UART_PORT,
                                  send_wait=SEND_WAIT, turnaround=TURNAROUND)
    for key, blocks in sec.items():
        if blocks:
            text += f"\n{key}:\n" + "".join(blocks)

    with open(YAML_FILE, "w", newline="\n") as f:
        f.write(text)
    print(f"Wrote {YAML_FILE}")


def main():
    global s
    s = serial.Serial(PORT, 115200, timeout=0.05)

    print("Scanning bus for assigned nodes...")
    devices = scan_assigned()
    print(f"Found {len(devices)} assigned node(s)")

    while True:
        uid = find_lowest_unassigned()
        if uid is None:
            print("No unassigned nodes remaining")
            break

        addr = next_free(devices)
        if addr is None:
            print("No available addresses remaining")
            break

        assign(uid, addr)
        answered, fid = probe(addr)
        if not answered or fid is None:
            print(f"Node {uid:016X} failed to respond at address {addr} after assignment, aborting")
            break

        devices[addr] = fid
        print(f"Assigned node {uid:016X} -> Address {addr}, FID {fid}")

    write_yaml(devices)
    s.close()


if __name__ == "__main__":
    main()
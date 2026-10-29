import json
import os
import time
import serial
import flavors
import headers

PORT           = "COM3"
TARGET         = "host"   # "host" | "wt32" | "s3zero"
HUB_NAME       = "hub-1"
YAML_FILE      = "hub.yaml"
CACHE_FILE     = "cache.json"
HOST_UART_PORT = "/dev/ttyUSB0" #real hardware now!
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


def anyone_answers(threshold: int, tries: int = 3) -> bool:
    for _ in range(tries):
        s.reset_input_buffer()
        s.write(frame("00 41 " + threshold.to_bytes(8, "big").hex(" ")))
        s.flush()
        if s.read(1):
            return True
        time.sleep(0.005)
    return False


def find_lowest_unassigned():
    if not anyone_answers(MAX_UID):
        return None
    for _ in range(3):
        low, high = 0, MAX_UID
        while low < high:
            mid = (low + high) // 2
            if anyone_answers(mid):
                high = mid
            else:
                low = mid + 1
            time.sleep(0.005)
        # the lowest UID must answer, and the number just below it must not
        if anyone_answers(low) and (low == 0 or not anyone_answers(low - 1)):
            return low
        print("search gave an inconsistent result, trying again")
    return None

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


# ---- cache -----------------------------------------------------------------
# cache.json keeps what the script has learned across runs:
#   { "<address>": { "uid": "<16 hex digits>", "fid": <int> } }
# An assigned node ignores broadcast traffic, so its UID can only be captured
# the run it is commissioned. A plain bus scan can refresh the FID at that
# address but can never recover a UID that was never recorded.

def cache_load() -> dict:
    try:
        with open(CACHE_FILE) as f:
            data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError, ValueError):
        return {}
    # JSON keys are always strings; convert them back to int addresses
    return {int(a): v for a, v in data.items()}


def cache_save(cache: dict):
    data = {str(a): cache[a] for a in sorted(cache)}
    tmp = CACHE_FILE + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, CACHE_FILE)


def cache_note_scan(cache: dict, devices: dict):
    """Fold a live bus scan into the cache. The live FID always wins; a UID
    already on record is kept, missing ones stay unknown (None)."""
    for addr, fid in devices.items():
        entry = cache.setdefault(addr, {"uid": None, "fid": None})
        if fid is not None:
            entry["fid"] = fid


def cache_note_assign(cache: dict, addr: int, uid: int, fid: int):
    """Record a freshly assigned node. This is the only moment the UID is
    available for an individual node."""
    cache[addr] = {"uid": f"{uid:016X}", "fid": fid}





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

    if TARGET == "s3zero":
        sec["light"].append(headers.BOARD_LED_S3ZERO)

    for n in sorted(devices):
        fid = devices[n]
        flavor = flavors.FLAVORS.get(fid)
        if flavor is None:
            print(f"Address {n}: FID {fid} has no mapping, skipping")
            continue

        name = f"{flavor['label']} {n}"

        if flavor["poll"] == "continuous":
            poll = ("    continuous: true\n"
                    "    update_interval: 1s\n"
                    "    offline_skip_updates: 9\n")
        elif flavor["poll"] == "periodic":
            poll = ("    update_interval: 1s\n"
                    "    offline_skip_updates: 9\n")
        else:
            poll = ""

        sec["modbus_controller"].append(
            f"  - id: node_{n}\n"
            f"    address: {n}\n"
            f"    modbus_id: modbus_bus\n" + poll)

        if fid == flavors.FID_SWITCH:
            sec["binary_sensor"].append(
                f"  - platform: modbus_controller\n"
                f"    modbus_controller_id: node_{n}\n"
                f"    name: \"{name}\"\n"
                f"    register_type: holding\n"
                f"    address: 1\n")
        elif fid in (flavors.FID_LIGHT_ONOFF, flavors.FID_PLUG):
            sec["switch"].append(
                f"  - platform: modbus_controller\n"
                f"    modbus_controller_id: node_{n}\n"
                f"    name: \"{name}\"\n"
                f"    register_type: holding\n"
                f"    address: 1\n"
                f"    bitmask: 1\n")
        elif fid == flavors.FID_LIGHT_DIM:
            sec["output"] += [output_block(n, "power", 1),
                              output_block(n, "brightness", 2, 1000)]
            sec["light"].append(light_block(n, name, "monochromatic",
                                            [f"output: node_{n}_brightness"]))
        elif fid == flavors.FID_TW_PWM:
            sec["output"] += [output_block(n, "power", 1),
                              output_block(n, "cold", 2, 1000),
                              output_block(n, "warm", 3, 1000)]
            sec["light"].append(light_block(n, name, "cwww", [
                f"cold_white: node_{n}_cold",
                f"warm_white: node_{n}_warm",
                f"cold_white_color_temperature: {COLD_K} K",
                f"warm_white_color_temperature: {WARM_K} K",
                "constant_brightness: true"]))

    text = headers.ALL[TARGET].format(name=HUB_NAME, port=HOST_UART_PORT,
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

    cache = cache_load()
    print(f"Loaded cache with {len(cache)} known node(s)")

    print("Scanning bus for assigned nodes...")
    devices = scan_assigned()
    print(f"Found {len(devices)} assigned node(s)")
    cache_note_scan(cache, devices)
    cache_save(cache)

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
        cache_note_assign(cache, addr, uid, fid)
        cache_save(cache)
        print(f"Assigned node {uid:016X} -> Address {addr}, FID {fid}")

    write_yaml(devices)
    s.close()


if __name__ == "__main__":
    main()
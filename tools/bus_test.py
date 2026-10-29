import json
import time
import serial
import flavors

PORT       = "COM3"
CACHE_FILE = "cache.json"

EXCEPTIONS = {1: "illegal function",
              2: "illegal address or read-only register",
              3: "value above the register's maximum"}

s = None


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def frame(hexstr: str) -> bytes:
    body = bytes.fromhex(hexstr)
    crc = crc16(body)
    return body + bytes([crc & 0xFF, crc >> 8])


def read_regs(addr: int, start: int = 0, count: int = 4):
    s.reset_input_buffer()
    s.write(frame(f"{addr:02X} 03 {start:04X} {count:04X}"))
    s.flush()
    n = 5 + 2 * count
    r = s.read(n)
    if len(r) != n or crc16(r) != 0 or r[0] != addr or r[1] != 0x03:
        return None
    return [(r[3 + 2 * i] << 8) | r[4 + 2 * i] for i in range(count)]


def reg_value(addr: int, reg: int):
    regs = read_regs(addr, reg, 1)
    return None if regs is None else regs[0]


def write_reg(addr: int, reg: int, value: int) -> bool:
    req = frame(f"{addr:02X} 06 {reg:04X} {value:04X}")
    s.reset_input_buffer()
    s.write(req)
    s.flush()
    r = s.read(8)
    if r == req:
        return True
    if len(r) == 5 and r[1] == 0x86:
        print(f"node refused the write: {EXCEPTIONS.get(r[2], hex(r[2]))}")
    else:
        print("no valid acknowledge")
    return False


def _forget_by_uid(uid: int):
    s.write(frame("00 43 " + uid.to_bytes(8, "big").hex(" ")))
    s.flush()
    time.sleep(0.05)


def cache_load() -> dict:
    try:
        with open(CACHE_FILE) as f:
            data = json.load(f)
        return {int(a): v for a, v in data.items()}
    except (FileNotFoundError, json.JSONDecodeError, ValueError):
        return {}


def show_regs():
    addr = int(input("address: "))
    fid = reg_value(addr, 0)
    raw = read_regs(addr)
    if raw is None:
        print("no valid reply")
        return
    flavor = flavors.FLAVORS.get(fid)
    reg_names = flavor["registers"] if flavor else {}
    for i, v in enumerate(raw):
        if i == 0:
            label = f"FID ({flavor['name']})" if flavor else "FID"
        else:
            label = reg_names[i]["name"] if i in reg_names else f"reg {i}"
        print(f"  reg {i}  {label:12s} = {v}")


def write_cmd():
    addr = int(input("address: "))
    reg = int(input("register: "))
    value = int(input("value: "))
    if write_reg(addr, reg, value):
        print(f"acknowledged, reg {reg} now reads {reg_value(addr, reg)}")


def on_off_check():
    addr = int(input("address: "))
    reg = int(input("register (2 = LED, 1 = relay): "))
    for value in (1, 0):
        if not write_reg(addr, reg, value):
            return
        print(f"reg {reg} reads {reg_value(addr, reg)}")
        input(f"should be {'on' if value else 'off'}, press Enter ")


def led_lease_test():
    addr = int(input("address: "))
    fid = reg_value(addr, 0)
    if fid not in (flavors.FID_SWITCH, flavors.FID_PLUG):
        print(f"FID {fid} has no identify LED")
        return

    print("blinking for 10 s")
    end = time.monotonic() + 10
    while time.monotonic() < end:
        for value in (1, 0):
            write_reg(addr, 2, value)
            time.sleep(0.5)

    print("leaving the LED on, it should switch itself off after about 30 s")
    if not write_reg(addr, 2, 1):
        return
    start = time.monotonic()
    while time.monotonic() - start < 40:
        time.sleep(0.5)
        if reg_value(addr, 2) == 0:
            print(f"register cleared after {time.monotonic() - start:.1f} s")
            return
    print("register still 1 after 40 s")


def watch():
    addr = int(input("address: "))
    reg = int(input("register: "))
    print("Ctrl+C to stop")
    last = -1
    try:
        while True:
            v = reg_value(addr, reg)
            if v != last:
                print(f"{time.strftime('%H:%M:%S')}  reg {reg} = {'no reply' if v is None else v}")
                last = v
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass


def forget_node():
    addr_str = input("address: ").strip()
    if not addr_str:
        print("address required")
        return
    addr = int(addr_str)

    uid = None
    cache = cache_load()
    entry = cache.get(addr)
    if entry and entry.get("uid"):
        uid_hex = entry["uid"]
        print(f"found UID {uid_hex} in cache")
        uid = int(uid_hex, 16)
    else:
        raw = input("UID not in cache -- enter 16 hex digits (or Enter to cancel): ").strip()
        if not raw:
            return
        uid = int(raw, 16)

    _forget_by_uid(uid)
    if reg_value(addr, 0) is None:
        print("node no longer answers, run discovery to reassign")
    else:
        print("node still answers at that address -- forget may have failed")


MENU = """
1  read the registers of a node
2  write a register
3  on/off check of a register
4  identify LED test (blink, then 30 s timeout)
5  watch a register
6  forget a node
q  quit"""

ACTIONS = {"1": show_regs, "2": write_cmd, "3": on_off_check,
           "4": led_lease_test, "5": watch, "6": forget_node}


def main():
    global s
    s = serial.Serial(PORT, 115200, timeout=0.05)
    while True:
        print(MENU)
        choice = input("> ").strip().lower()
        if choice == "q":
            break
        action = ACTIONS.get(choice)
        if action is None:
            continue
        try:
            action()
        except ValueError:
            print("could not read that number")
    s.close()


if __name__ == "__main__":
    main()

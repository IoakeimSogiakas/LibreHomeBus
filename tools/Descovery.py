import time
import serial

# --- Helpers ---
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
        out = out[:-1] + bytes([out[-1] ^ 0xFF])  # corrupt the last CRC byte
    return out

# --- Port Setup ---
# Note: timeout is set to 0.05 for fast discovery polling
s = serial.Serial("COM5", 115200, timeout=0.05)
MAX_UID = 2**64 - 1

def anyone_answers(threshold: int) -> bool:
    s.reset_input_buffer()
    s.write(frame("00 41 " + threshold.to_bytes(8, "big").hex(" ")))
    s.flush()
    return len(s.read(1)) > 0           # any byte at all counts as "yes"

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

def read_reg0(addr: int):
    s.reset_input_buffer()
    s.write(frame(f"{addr:02X} 03 00 00 00 01"))
    s.flush()
    r = s.read(7)
    if len(r) != 7 or crc16(r) != 0:
        return None
    return (r[3] << 8) | r[4]

# --- Main Discovery Loop ---
next_addr = 1
while True:
    uid = find_lowest_unassigned()
    if uid is None:
        print("no unassigned chips left")
        break
    assign(uid, next_addr)
    print(f"found {uid:016X} -> address {next_addr}, reg 0 reads {read_reg0(next_addr)}")
    next_addr += 1

s.close()
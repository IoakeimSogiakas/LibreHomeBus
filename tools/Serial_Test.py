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
s = serial.Serial("COM5", 115200, timeout=0.2)

def read_regs(start: int, count: int):
    """Ask slave 1 for `count` registers starting at `start`.
    Returns a list of numbers, or None if the reply was missing or damaged."""
    s.reset_input_buffer()
    s.write(frame(f"01 03 {start >> 8:02X} {start & 0xFF:02X} {count >> 8:02X} {count & 0xFF:02X}"))
    s.flush()
    r = s.read(5 + 2 * count)                   # address + function + byte count + data + 2 CRC
    time.sleep(0.03)                            # allow slave debug print to clear
    if len(r) != 5 + 2 * count or crc16(r) != 0:
        return None
    return [(r[3 + 2 * i] << 8) | r[4 + 2 * i] for i in range(count)]

def write_reg(reg: int, value: int):
    """Write one register on slave 1. Returns True if the slave echoed our request back."""
    req = frame(f"01 06 {reg:04X} {value:04X}")
    s.reset_input_buffer()
    s.write(req)
    s.flush()
    res = s.read(8) == req
    time.sleep(0.03)                            # allow slave debug print to clear
    return res

print(write_reg(1, 1), read_regs(0, 3))     # on: expect True [3, 1, 1000]
time.sleep(1)
for b in range(0, 1001, 50):                # fade up
    write_reg(2, b)
    time.sleep(0.05)
write_reg(2, 100)                           # dim
time.sleep(1)
write_reg(1, 0)                             # off
print(write_reg(0, 5))                      # writing the FID: expect False (no reply, it's read-only)
print(write_reg(2, 5000))                   # out of range: expect False

try:
    while True:                             # Ctrl+C to stop
        print(read_regs(0, 2))              # expect [1, switch_state]
        time.sleep(0.2)
except KeyboardInterrupt:
    pass
finally:
    s.close()
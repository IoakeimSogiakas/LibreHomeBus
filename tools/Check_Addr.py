import serial

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

s = serial.Serial("COM5", 115200, timeout=0.2)

def read_reg0(addr: int):
    s.reset_input_buffer()
    s.write(frame(f"{addr:02X} 03 00 00 00 01"))
    s.flush()
    r = s.read(7)
    if len(r) != 7 or crc16(r) != 0:
        return None
    return (r[3] << 8) | r[4]

for addr in (1, 2):
    print(f"address {addr}: reg 0 = {read_reg0(addr)}")

s.close()
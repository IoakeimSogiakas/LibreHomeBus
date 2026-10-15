import time, serial

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc

def frame(hexstr):
    body = bytes.fromhex(hexstr)
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])

s = serial.Serial("COM5", 115200, timeout=0.2)
uid = 0xCDAB66AC4BBC7E14
s.write(frame("00 42 " + uid.to_bytes(8, "big").hex(" ") + " 01"))
s.flush()
time.sleep(0.1)
for addr in (1, 2):
    s.reset_input_buffer()
    s.write(frame(f"{addr:02X} 03 00 00 00 01"))
    s.flush()
    r = s.read(7)
    print(addr, (r[3] << 8) | r[4] if len(r) == 7 and crc16(r) == 0 else None)
import serial
import struct
import time

PORT = "COM5"
BAUD = 921600
MAGIC = 0xDEADBEEF

# 1. Open port WITHOUT toggling DTR/RTS (prevents ESP reset)
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 30
ser.dtr = False   # <-- key: don't pull DTR low on open
ser.rts = False   # <-- key: don't pull RTS low on open
ser.open()

ser.reset_input_buffer()

# 2. Wait for ESP to print its READY marker before triggering
print("[+] Waiting for ESP to be ready...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    print(f"  ESP: {line}")
    if "READY" in line or "trigger" in line.lower():
        break

print("[+] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# --- rest of your receive logic below ---
def read_exact(ser, size):
    data = b''
    ser.timeout = 30  # long timeout per read
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"Stalled: got {len(data)}/{size} bytes")
        data += chunk
        print(f"  Progress: {len(data)}/{size} bytes ({100*len(data)//size}%)", end='\r')
    print()
    return data
    

def find_header(ser):
    buffer = b''
    print("[+] Scanning for magic...")
    while True:
        b = ser.read(1)
        if not b:
            continue
        buffer += b
        if len(buffer) >= 4:
            val = struct.unpack("<I", buffer[-4:])[0]
            if val == MAGIC:
                rest = read_exact(ser, 9 - 4)   # IHHB = 4+2+2+1 = 9 bytes total
                print("✅ MAGIC FOUND")
                return buffer[-4:] + rest

header = find_header(ser)
magic, width, height, fmt = struct.unpack("<IHHB", header)
print(f"[+] Image: {width}x{height} format={fmt}")

img_size = width * height * 2
print(f"[+] Reading {img_size} bytes...")
data = read_exact(ser, img_size)
print(f"[+] Done! Saving...")

import numpy as np, cv2
with open("out.raw", "wb") as f:
    f.write(data)

img = np.frombuffer(data, dtype=np.uint16).reshape((height, width))
r = ((img >> 11) & 0x1F) << 3
g = ((img >> 5)  & 0x3F) << 2
b = (img & 0x1F) << 3
cv2.imwrite("out.png", np.dstack((b, g, r)).astype(np.uint8))
print("✅ Saved out.png")
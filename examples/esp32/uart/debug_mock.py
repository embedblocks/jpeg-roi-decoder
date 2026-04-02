import serial, time, struct, numpy as np

PORT  = "COM5"
MAGIC = 0xDEADBEEF
W, H  = 320, 240

ser = serial.Serial(PORT, 115200, timeout=30)
ser.dtr = False
ser.rts = False

# ── Step 1: Handshake at 115200 ───────────────────────────────────────────────
print("[1] Waiting for READY at 115200...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    print(f"  ESP: {line}")
    if "READY" in line:
        break

ser.reset_input_buffer()
print("[2] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# Watch for the confirmation log before switching baud
ser.timeout = 5
for _ in range(10):
    line = ser.readline().decode(errors='replace').strip()
    print(f"  ESP: {line}")
    if "Trigger" in line:
        print("  ✅ ESP confirmed trigger received")
        break

# ── Step 2: Switch baud in-place, NO close/reopen ────────────────────────────
time.sleep(0.3)                  # let ESP32 finish its last 115200 log line
ser.baudrate = 921600            # change baud on the SAME open port
ser.reset_input_buffer()         # discard any garbage during transition
print("[3] Switched to 921600 on same port — scanning for magic...")

# ── Step 3: Scan for magic ────────────────────────────────────────────────────
ser.timeout = 15
buf = b''
while True:
    b = ser.read(1)
    if not b:
        print("  TIMEOUT — no magic received")
        break
    buf += b
    if len(buf) >= 4 and struct.unpack("<I", buf[-4:])[0] == MAGIC:
        print("  MAGIC found!")
        break

# ── Step 4: Read rest of header ───────────────────────────────────────────────
rest  = ser.read(5)
magic, width, height, fmt = struct.unpack("<IHHB", buf[-4:] + rest)
print(f"[4] Header: {width}x{height} fmt={fmt}")

# ── Step 5: Read pixels ───────────────────────────────────────────────────────
total = width * height * 2
print(f"[5] Reading {total} bytes...")
data = b''
while len(data) < total:
    chunk = ser.read(total - len(data))
    data += chunk
    print(f"  {len(data)}/{total}", end='\r')

print(f"\n[6] Got {len(data)} bytes — verifying...")
pixels   = np.frombuffer(data, dtype=np.uint16).reshape((H, W))
xs       = np.arange(W, dtype=np.uint16)
ys       = np.arange(H, dtype=np.uint16)
expected = (((xs[np.newaxis,:] // 10) & 0x1F) << 11 | \
            ((ys[:,np.newaxis] //  4) & 0x3F) << 5).astype(np.uint16)
errors   = np.sum(pixels.astype(np.int32) != expected.astype(np.int32))
print(f"[7] Pixel errors: {errors}/{W*H}")
if errors == 0:
    print("    PERFECT — transport is clean.")
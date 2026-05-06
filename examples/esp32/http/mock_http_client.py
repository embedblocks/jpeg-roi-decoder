import ssl
import socket

URL_HOST = "i.gzn.jp"
URL_PATH = "/img/2009/06/18/lenna/000.jpg"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

# ==========================================
# 1. Setup Raw TLS Connection (No HTTP client library)
# ==========================================
print("Connecting...")
ctx = ssl.create_default_context()
sock = socket.create_connection((URL_HOST, 443), timeout=10)
ssock = ctx.wrap_socket(sock, server_hostname=URL_HOST)

req = (
    f"GET {URL_PATH} HTTP/1.0\r\n"  # <--- CHANGED TO 1.0
    f"Host: {URL_HOST}\r\n"
    f"User-Agent: {UA}\r\n"
    "\r\n"
)
ssock.sendall(req.encode())

# ==========================================
# 2. Read Headers (Mocking esp_http_client_fetch_headers)
# ==========================================
print("Reading headers...")
headers = b""
while b"\r\n\r\n" not in headers:
    headers += ssock.recv(1)

header_str = headers.decode(errors='ignore').split("\r\n\r\n")[0]
print("--- HEADERS ---")
print(header_str)
print("----------------")

# ==========================================
# 3. Mock ESP-IDV http_read_cb (buffer_size=2048)
# ==========================================
esp_http_buf = b""

def esp_http_read(max_size):
    global esp_http_buf
    if len(esp_http_buf) >= max_size:
        data = esp_http_buf[:max_size]
        esp_http_buf = esp_http_buf[max_size:]
        return data
        
    # THIS IS WHAT ESP-IDF DOES: Tries to greedily fill a 2048 bucket
    try:
        recv_size = 2048 - len(esp_http_buf)
        print(f"   [HTTP Client] Socket recv({recv_size})...", end="", flush=True)
        chunk = ssock.recv(recv_size)
        if not chunk:
            print("EOF")
            return b""
        print(f"Got {len(chunk)}")
        esp_http_buf += chunk
        
        data = esp_http_buf[:max_size]
        esp_http_buf = esp_http_buf[max_size:]
        return data
    except ssl.SSLError as e:
        print(f"\n!!! SSL ERROR (Equivalent to ESP -0x7100): {e} !!!")
        return b""

# ==========================================
# 4. Mock Component Internal Buffer
# ==========================================
comp_buf = b""

def component_read(max_size):
    global comp_buf
    if len(comp_buf) >= max_size:
        data = comp_buf[:max_size]
        comp_buf = comp_buf[max_size:]
        return data

    # Component asks HTTP for 2048
    needed_from_http = 2048 - len(comp_buf)
    http_data = esp_http_read(needed_from_http)
    if not http_data:
        return b""
        
    comp_buf += http_data
    data = comp_buf[:max_size]
    comp_buf = comp_buf[max_size:]
    return data

# ==========================================
# 5. Mock Decoder Sequence (From your logs)
# ==========================================
print("\n--- Starting Decode Stream ---")
sequence = [
    1, 1, 4, ("skip", 14), 4, 555, 
    2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048
]
total = 0

for item in sequence:
    if isinstance(item, tuple):
        op, size = item
        print(f"Decoder: Skip {size} bytes")
        component_read(size)
        total += size
    else:
        print(f"Decoder: Read {item} bytes")
        data = component_read(item)
        if not data:
            print(">>> STREAM DIED! <<<")
            break
        total += len(data)

print(f"\nFinished successfully. Total bytes: {total}")
ssock.close()
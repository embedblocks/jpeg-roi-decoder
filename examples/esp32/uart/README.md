# JPEG UART Streaming Example (ESP-IDF)

This example demonstrates how to stream a decoded JPEG image from an ESP32 to a PC over UART using a **row-wise (chunked) callback mechanism**, and reconstruct it on the host side using Python.

---

## 🎯 Overview

The ESP32:

1. Waits for a trigger from the PC  
2. Switches UART baud rate for high-speed transfer  
3. Sends a small binary **header** (metadata)  
4. Streams raw pixel data (RGB565) via callback  

The PC:

1. Sends trigger  
2. Switches baud rate  
3. Waits for header  
4. Receives raw image bytes  
5. Reconstructs and saves the image  

---

# UART JPEG Streaming Example — Usage

This guide explains how to run the UART image streaming example using:

- ESP32 (sender)
- Python script (receiver)

---

## 📟 ESP32 Side (Sender)

### 1. Build & Flash

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor


## 📟 PC Side (Receiver)
python image_rcv.py
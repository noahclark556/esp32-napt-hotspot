# Basic Internet Sharing Example

This example demonstrates how to use the ESP32 NAPT component to turn your ESP32 into a Wi-Fi hotspot with full internet sharing.

## What This Example Does

1. Connects the ESP32 to your existing Wi-Fi router in Station (STA) mode
2. Creates a Wi-Fi Access Point (AP) for other devices to connect to
3. Enables NAPT to share internet access with connected clients
4. Starts a DNS forwarder so clients can resolve domains without manual setup

Devices that connect to the ESP32 hotspot will automatically receive full internet access.

---

## Setup Instructions

### 1. Set Your Wi-Fi Credentials

Edit `main.c` and update the following:

```c
#define WIFI_SSID         "YourRouterSSID"
#define WIFI_PASSWORD     "YourRouterPassword"
#define HOTSPOT_SSID      "ESP32-Hotspot"
#define HOTSPOT_PASSWORD  "myhotspot123"
```

### 2. Ensure Build Configuration is Set

Make sure your project has the required lwIP settings enabled. Refer to the main README for full configuration instructions.

---

## Building with ESP-IDF

```bash
cd examples/basic
idf.py set-target esp32s3        # or use esp32, esp32c3, etc.
idf.py menuconfig                # Enable IP forwarding, NAPT, etc.
idf.py build
idf.py flash monitor
```

---

## Building with PlatformIO

Create a `platformio.ini` file in your project root:

```ini
[env:esp32s3]
platform = espressif32
framework = espidf
board = esp32-s3-devkitc-1

build_flags = 
    -DCONFIG_LWIP_IP_FORWARD=1
    -DCONFIG_LWIP_IPV4_NAPT=1
    -DIP_FORWARD_ALLOW_TX_ON_RX_NETIF=1
```

Then build and upload:

```bash
pio run -t upload && pio device monitor
```

---

## Example Output

When running, you should see log output similar to:

```
ESP32 NAPT Basic Example
Connecting to WiFi router...
Connected to WiFi! IP: 192.168.1.100
✓ Connected to WiFi: YourRouterSSID

Enabling WiFi hotspot with internet sharing...
AP interface ready: 192.168.4.1
NAT enabled successfully
DNS forwarder started
Hotspot enabled successfully

✓ Hotspot is READY!
SSID:     ESP32-Hotspot
Password: myhotspot123
IP:       192.168.4.1
DNS:      Automatic (forwarded)
NAPT:     Enabled (internet sharing)
```

---

## Testing the Hotspot

Once the hotspot is active:

1. Connect a device (phone/laptop) to `ESP32-Hotspot` using the password you set
2. The device should automatically receive an IP address like `192.168.4.x`
3. Try browsing the web — it should work if the ESP32 is connected to the internet

### Optional Connection Tests

From your connected device:

```bash
ping 192.168.4.1     # Ping the ESP32 hotspot
ping 8.8.8.8         # Test internet access via NAPT
ping google.com      # Test DNS + internet access
```
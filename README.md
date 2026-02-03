# ESP32 NAPT Internet Sharing

This component turns your ESP32 into a Wi-Fi hotspot that shares internet access via NAT/NAPT. Clients connected to the ESP32's access point will automatically have internet access, with no extra setup required on their end. DNS forwarding is also included, so domain names should resolve without a hitch. No firmware flashing is required to use this package.

## Context

This component was originally created for a project I was working on. At the time, I struggled to find any existing repositories that worked with the latest ESP-IDF versions. I decided to circle back and share this solution in case it helps others. I may also port it to the Arduino framework in the future, where it could be useful for a wider range of projects.

That said, since this wasn’t originally intended for public use, I may have missed a few details in the documentation. However, this README should cover most of what you need to get started. Contributions and improvements are always welcome!

## Features

* Internet sharing using NAT/NAPT
* DNS forwarding so clients can resolve domain names
* Plug-and-play for connected devices
* Non-blocking, runs in background tasks
* Simple API with minimal setup
* Works with both ESP-IDF and PlatformIO

## How It Works

Your ESP32 connects to your router in STA (station) mode, and creates its own Wi-Fi hotspot in AP (access point) mode. Connected clients use the ESP32 as a gateway to access the internet.

```
[Internet] ←→ [Router] ←→ [ESP32 STA Interface]
                              ↑
                        [ESP32 AP]
                              ↑
                  [Connected Clients]
```

## Requirements

**Hardware:**

* Developed and tested primarily on ESP32-S3, but can be adjusted to work with other ESP32 models

**Software:**

* ESP-IDF v5.3.1 (may work on ESP-IDF 5.0+)
* Compatible with ESP-IDF and PlatformIO build systems

## Installation

### ESP-IDF (idf.py)

You may clone this repo and integrate manually, or add the folowwing to your `idf_component.yml`:

```yaml
dependencies:
  noahclark556/esp32-napt-hotspot:
    version: "^1.0.2"
```

Followed by the following command:
```bash
idf.py reconfigure
```

### PlatformIO

Add to your `platformio.ini`:

```ini
[env:esp32s3]
platform = espressif32
framework = espidf
board = esp32-s3-devkitc-1

lib_deps = 
    https://github.com/noahclark556/esp32-napt-hotspot.git

build_flags = 
    -DCONFIG_LWIP_IP_FORWARD=1
    -DCONFIG_LWIP_IPV4_NAPT=1
    -DIP_FORWARD_ALLOW_TX_ON_RX_NETIF=1
```

## Required Configuration

### ESP-IDF

Run `idf.py menuconfig` and enable the following:

```
Component config → LWIP → 
    Enable IP forwarding = YES
    Enable NAT (experimental) = YES
    Enable NAPT = YES
```

You can also copy the included `sdkconfig.defaults` file into your project root.

### PlatformIO

Add these build flags to your `platformio.ini`:

```ini
build_flags = 
    -DCONFIG_LWIP_IP_FORWARD=1
    -DCONFIG_LWIP_IPV4_NAPT=1
    -DIP_FORWARD_ALLOW_TX_ON_RX_NETIF=1
```

The last flag is critical to allow forwarding between STA and AP interfaces.

## Example Usage

```c
#include "napt_interface.h"

void app_main(void) {
    // Initialize NVS and WiFi
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_wifi_init(&WIFI_INIT_CONFIG_DEFAULT());

    // Create STA interface and connect to Wi-Fi
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta_config = {
        .sta = {
            .ssid = "YourRouterSSID",
            .password = "YourRouterPassword",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();
    esp_wifi_connect();

    // Wait a few seconds for connection
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Start the hotspot
    enable_hotspot("ESP32-Hotspot", "myhotspot123");
}
```

## API Reference

### `enable_hotspot(const char *ssid, const char *password)`

Starts the access point and enables NAPT routing and DNS forwarding. If either argument is `NULL`, it uses default values from Kconfig.

### `disable_hotspot()`

Stops internet sharing and disables the access point. Does not disconnect from the STA Wi-Fi.

### `is_hotspot_enabled()`

Returns `true` if the hotspot is currently running, `false` otherwise.

## Configuration Options

All options can be changed via `menuconfig` under "ESP32 NAPT Configuration":

| Option           | Default       | Description                          |
| ---------------- | ------------- | ------------------------------------ |
| Default SSID     | ESP32-Hotspot | Default hotspot network name         |
| Default Password | esp32hotspot  | Default hotspot password             |
| WiFi Channel     | 1             | Wi-Fi channel (1–13)                 |
| Max Connections  | 4             | Number of clients allowed to connect |

Network configuration:

| Setting       | Value          |
| ------------- | -------------- |
| AP IP Address | 192.168.4.1    |
| Subnet        | 192.168.4.0/24 |
| DHCP Range    | .2 to .254     |
| Gateway       | 192.168.4.1    |
| DNS           | Auto forwarded |

## Troubleshooting

**Clients can’t connect?**

* Make sure ESP32 is connected to your router
* Hotspot password must be at least 8 characters

**Clients connect but no internet?**

* Ensure required build flags are set (especially `IP_FORWARD_ALLOW_TX_ON_RX_NETIF`)
* Confirm ESP32 has internet access in STA mode
* Check logs for successful NAT/DNS startup

**DNS not working?**

* Make sure the DNS forwarder task is running
* Reconnect the client to refresh DHCP

## Performance & Limitations

* Minimal latency for NAT (typically under 1ms)
* DNS forwarding adds one small hop
* Suitable for browsing and general use
* Max 4 clients by default (can increase in config)
* NAT table size is limited by lwIP memory
* DNS forwarder is single-threaded

## License

MIT License — see [LICENSE](LICENSE) for details.

## Support

* Issues: [GitHub Issues](https://github.com/noahclark556/esp32-napt-hotspot/issues)
* Questions: [GitHub Discussions](https://github.com/noahclark556/esp32-napt-hotspot/discussions)

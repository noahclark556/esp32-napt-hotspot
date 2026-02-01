/*
 * ESP32 NAPT - Basic Example
 * 
 * This example demonstrates the basic usage of the ESP32 NAPT component
 * to create a WiFi hotspot with full internet sharing.
 * 
 * What this example does:
 * 1. Connects to your WiFi router (STA mode)
 * 2. Creates a WiFi hotspot (AP mode)
 * 3. Enables NAPT for internet sharing
 * 4. Starts DNS forwarder for automatic DNS resolution
 * 
 * Devices connecting to the ESP32's hotspot will automatically get
 * internet access with zero configuration required!
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "napt_interface.h"

// ============================================================================
// CONFIGURATION - Modify these for your setup
// ============================================================================

// Your WiFi router credentials (ESP32 will connect as a client)
#define WIFI_SSID      "YourRouterSSID"
#define WIFI_PASSWORD  "YourRouterPassword"

// Hotspot credentials (devices will connect to this)
#define HOTSPOT_SSID      "ESP32-Hotspot"
#define HOTSPOT_PASSWORD  "myhotspot123"  // Must be at least 8 characters

// ============================================================================

static const char *TAG = "napt_example";

// Event group for WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define MAX_RETRY 5

// ============================================================================
// WiFi Event Handler
// ============================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to WiFi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Failed to connect to WiFi");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected to WiFi! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ============================================================================
// Initialize and Connect to WiFi (Station Mode)
// ============================================================================
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create STA network interface
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ Connected to WiFi: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "✗ Failed to connect to WiFi: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "✗ Unexpected event");
    }
}

// ============================================================================
// Main Application
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32 NAPT Basic Example");
    ESP_LOGI(TAG, "========================================");

    // Step 1: Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Step 2: Initialize and connect to WiFi (Station mode)
    ESP_LOGI(TAG, "Step 1: Connecting to WiFi router...");
    wifi_init_sta();

    // Wait a bit to ensure connection is stable
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Step 3: Enable hotspot with internet sharing!
    ESP_LOGI(TAG, "Step 2: Enabling WiFi hotspot with internet sharing...");
    enable_hotspot(HOTSPOT_SSID, HOTSPOT_PASSWORD);

    if (is_hotspot_enabled()) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "✓ Hotspot is READY!");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "SSID:     %s", HOTSPOT_SSID);
        ESP_LOGI(TAG, "Password: %s", HOTSPOT_PASSWORD);
        ESP_LOGI(TAG, "IP:       192.168.4.1");
        ESP_LOGI(TAG, "DNS:      Automatic (forwarded)");
        ESP_LOGI(TAG, "NAPT:     Enabled (internet sharing)");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Connect your device to '%s' and enjoy internet!", HOTSPOT_SSID);
        ESP_LOGI(TAG, "========================================");
    } else {
        ESP_LOGE(TAG, "Failed to enable hotspot!");
    }
}

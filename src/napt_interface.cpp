/***************************************************************************************
 *  File        : napt_interface.cpp
 *  Description : ESP32 internet sharing with NAT and DNS forwarding
 *  Author      : Noah Clark
 *  Created     : 2026-01-29
 *--------------------------------------------------------------------------------------
 *  Part of the QC Smartwatch Firmware
 *--------------------------------------------------------------------------------------
 *  Notes:
 *   - Relevant build flags and sdkconfig settings are needed for this to work properly.
 ***************************************************************************************/


#include <string.h>
#include "napt_interface.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

// Default configuration values - can be overridden via sdkconfig
#ifndef DEFAULT_HOTSPOT_SSID
#define DEFAULT_HOTSPOT_SSID "ESP32-Hotspot"
#endif

#ifndef DEFAULT_HOTSPOT_PASSWORD
#define DEFAULT_HOTSPOT_PASSWORD "esp32hotspot"
#endif

#ifndef HOTSPOT_CHANNEL
#define HOTSPOT_CHANNEL 1
#endif

#ifndef HOTSPOT_MAX_CONNECTIONS
#define HOTSPOT_MAX_CONNECTIONS 4
#endif

static const char *TAG = "napt_interface";


// ============================================================================
// HOTSPOT STATE
// ============================================================================
static bool hotspot_enabled = false;
static esp_netif_t *ap_netif = NULL;

// NAT (Network Address Translation) state for internet sharing
static bool napt_enabled = false;
static uint32_t napt_address = 0;  // Track which IP address NAT is enabled on

// ============================================================================
// DNS FORWARDER STATE
// ============================================================================
static int dns_forwarder_socket = -1;
static TaskHandle_t dns_forwarder_task_handle = NULL;
static ip_addr_t upstream_dns;  // Upstream DNS server to forward queries to

// ============================================================================
// NAT SUPPORT FUNCTIONS
// ============================================================================
// NAT support functions from lwIP
// These functions enable Network Address Translation for internet sharing
extern "C" {
    void ip_napt_enable(uint32_t addr, int enable);
}

// ============================================================================
// DNS FORWARDER TASK
// ============================================================================
// This task runs a transparent DNS proxy on the ESP32's AP interface.
// It listens on port 53 (DNS) and forwards all DNS queries from hotspot
// clients to the upstream DNS server (router's DNS or 8.8.8.8).
// This allows clients to resolve domain names without manual DNS configuration.
//
// How it works:
// 1. Client (e.g., phone) sends DNS query to 192.168.4.1:53
// 2. ESP32 receives query and forwards it to upstream DNS (e.g., 8.8.8.8:53)
// 3. ESP32 receives response from upstream DNS
// 4. ESP32 forwards response back to client
// ============================================================================
static void dns_forwarder_task(void *pvParameters)
{
    static char rx_buffer[512];
    static char tx_buffer[512];
    struct sockaddr_in dest_addr;    // Upstream DNS server address
    struct sockaddr_in source_addr;  // Client address
    socklen_t socklen = sizeof(source_addr);
    
    ESP_LOGI(TAG, "DNS Forwarder: Starting on port 53");
    
    // Create UDP socket for DNS (port 53)
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS Forwarder: Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // Bind socket to port 53 on all interfaces
    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(53);  // DNS port
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Listen on all interfaces
    
    int err = bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "DNS Forwarder: Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS Forwarder: Listening on 0.0.0.0:53");
    ESP_LOGI(TAG, "DNS Forwarder: Forwarding to " IPSTR, IP2STR(&upstream_dns.u_addr.ip4));
    
    dns_forwarder_socket = sock;
    
    // Set receive timeout so we can check hotspot_enabled periodically
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    
    // Main DNS forwarding loop - runs while hotspot is enabled
    while (hotspot_enabled) {
        // Receive DNS query from client
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, 
                          (struct sockaddr *)&source_addr, &socklen);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - just continue to check hotspot_enabled
                continue;
            }
            ESP_LOGE(TAG, "DNS Forwarder: recvfrom failed: errno %d", errno);
            break;
        }
        
        if (len > 0) {
            // Forward DNS query to upstream DNS server
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(53);
            dest_addr.sin_addr.s_addr = upstream_dns.u_addr.ip4.addr;
            
            // Create new socket for upstream query
            int upstream_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (upstream_sock >= 0) {
                // Set timeout for upstream query (2 seconds)
                struct timeval upstream_timeout;
                upstream_timeout.tv_sec = 2;
                upstream_timeout.tv_usec = 0;
                setsockopt(upstream_sock, SOL_SOCKET, SO_RCVTIMEO, &upstream_timeout, sizeof upstream_timeout);
                
                // Send query to upstream DNS
                sendto(upstream_sock, rx_buffer, len, 0, 
                      (struct sockaddr *)&dest_addr, sizeof(dest_addr));
                
                // Receive response from upstream DNS
                int response_len = recvfrom(upstream_sock, tx_buffer, sizeof(tx_buffer) - 1, 0, NULL, NULL);
                
                if (response_len > 0) {
                    // Forward response back to original client
                    sendto(sock, tx_buffer, response_len, 0, 
                          (struct sockaddr *)&source_addr, socklen);
                }
                
                close(upstream_sock);
            }
        }
    }
    
    // Cleanup
    close(sock);
    dns_forwarder_socket = -1;
    ESP_LOGI(TAG, "DNS Forwarder: Stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// PUBLIC API IMPLEMENTATION
// ============================================================================

// ============================================================================
// enable_hotspot()
// ============================================================================
// Enables the ESP32 as a WiFi hotspot with full internet sharing via NAT.
// 
// Prerequisites:
// - ESP32 must be connected to WiFi (STA mode) first
// - This provides the internet connection to share
//
// What this function does:
// 1. Creates an Access Point (AP) network interface
// 2. Configures the AP with SSID, password, and IP (192.168.4.1)
// 3. Switches WiFi to APSTA mode (both client and hotspot simultaneously)
// 4. Enables NAT on the AP address for internet sharing
// 5. Starts a DNS forwarder for automatic DNS resolution
//
// Network topology after enabling:
// [Internet] <-> [Router] <-> [ESP32 STA: 192.168.1.x] 
//                              [ESP32 AP: 192.168.4.1] <-> [Clients: 192.168.4.x]
//
// NAT translates packets between 192.168.4.x (clients) and 192.168.1.x (internet)
// ============================================================================
void enable_hotspot(const char *ssid, const char *password)
{
    // Check if hotspot is already running
    if (hotspot_enabled)
    {
        ESP_LOGI(TAG, "Hotspot already enabled");
        return;
    }

    // Verify we're connected to WiFi - this is required for internet sharing
    // Check if STA interface exists and has IP
    esp_netif_t *sta_check = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t sta_check_ip;
    if (sta_check == NULL || esp_netif_get_ip_info(sta_check, &sta_check_ip) != ESP_OK || sta_check_ip.ip.addr == 0)
    {
        ESP_LOGE(TAG, "Must be connected to WiFi (STA mode) before enabling hotspot");
        return;
    }

    ESP_LOGI(TAG, "Enabling hotspot: %s", ssid ? ssid : DEFAULT_HOTSPOT_SSID);

    // Step 1: Create AP network interface if it doesn't exist
    if (ap_netif == NULL)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        if (ap_netif == NULL)
        {
            ESP_LOGE(TAG, "Failed to create AP network interface");
            return;
        }
        
        // Get DNS server from STA interface (or use Google DNS as fallback)
        // This will be used by the DNS forwarder
        esp_netif_dns_info_t dns_info;
        
        // Try to get DNS from the STA interface (router's DNS)
        esp_netif_t *sta_netif_for_dns = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta_netif_for_dns != NULL && 
            esp_netif_get_dns_info(sta_netif_for_dns, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.u_addr.ip4.addr != 0)
        {
            ESP_LOGI(TAG, "Using STA's DNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }
        else
        {
            // Fallback to Google DNS (8.8.8.8) if STA DNS not available
            IP4_ADDR(&dns_info.ip.u_addr.ip4, 8, 8, 8, 8);
            ESP_LOGI(TAG, "Using fallback DNS: 8.8.8.8");
        }
        
        // Configure DHCP server to advertise DNS to clients
        // Note: This sets what DNS the DHCP server tells clients to use
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, 
                               &dns_info.ip.u_addr.ip4.addr, sizeof(dns_info.ip.u_addr.ip4.addr));
        
        // Step 2: Configure AP IP address and DHCP settings
        esp_netif_dhcps_stop(ap_netif);  // Stop DHCP to reconfigure
        
        esp_netif_ip_info_t ap_ip_config;
        IP4_ADDR(&ap_ip_config.ip, 192, 168, 4, 1);        // AP IP: 192.168.4.1
        IP4_ADDR(&ap_ip_config.gw, 192, 168, 4, 1);        // Gateway: 192.168.4.1 (self)
        IP4_ADDR(&ap_ip_config.netmask, 255, 255, 255, 0); // Subnet: 192.168.4.0/24
        esp_netif_set_ip_info(ap_netif, &ap_ip_config);
        
        esp_netif_dhcps_start(ap_netif);  // Restart DHCP server
        ESP_LOGI(TAG, "AP configured: IP=192.168.4.1, Gateway=192.168.4.1");
    }

    // Step 3: Switch WiFi to APSTA mode (both Station and Access Point)
    // This allows ESP32 to be connected to WiFi AND act as a hotspot simultaneously
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
        return;
    }
    
    // Allow WiFi stack to stabilize after mode change
    vTaskDelay(pdMS_TO_TICKS(500));

    // Step 4: Configure Access Point settings (SSID, password, channel, etc.)
    wifi_config_t ap_config = {};
    
    // Set SSID (network name)
    const char *ap_ssid = ssid ? ssid : DEFAULT_HOTSPOT_SSID;
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);
    
    // Set password and security mode
    if (password && strlen(password) >= 8)
    {
        // Use provided password (must be at least 8 characters)
        strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else if (strlen(DEFAULT_HOTSPOT_PASSWORD) >= 8)
    {
        // Use default password
        strncpy((char *)ap_config.ap.password, DEFAULT_HOTSPOT_PASSWORD, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        // No valid password - create open network (not recommended)
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ap_config.ap.channel = HOTSPOT_CHANNEL;
    ap_config.ap.max_connection = HOTSPOT_MAX_CONNECTIONS;
    ap_config.ap.beacon_interval = 100;

    // Apply AP configuration to WiFi driver
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Hotspot configuration applied, waiting for AP interface...");
    
    // Step 5: Wait for AP interface to be fully initialized with IP address
    int retry = 0;
    uint32_t ap_addr = 0;
    esp_netif_ip_info_t ap_ip_info;
    
    while (retry < 20)  // Try for up to 2 seconds (20 * 100ms)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        
        if (esp_netif_get_ip_info(ap_netif, &ap_ip_info) == ESP_OK)
        {
            ap_addr = ap_ip_info.ip.addr;
            if (ap_addr != 0)
            {
                ESP_LOGI(TAG, "AP interface ready: " IPSTR, IP2STR(&ap_ip_info.ip));
                break;
            }
        }
        retry++;
    }
    
    if (ap_addr == 0)
    {
        ESP_LOGE(TAG, "AP interface failed to get IP address");
        return;
    }

    // Step 6: Get STA (Station) interface information
    // The STA interface is our connection to the internet via the router
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to get STA network interface");
        return;
    }

    esp_netif_ip_info_t sta_ip_info;
    if (esp_netif_get_ip_info(sta_netif, &sta_ip_info) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get STA IP info");
        return;
    }

    uint32_t sta_addr = sta_ip_info.ip.addr;
    if (sta_addr == 0)
    {
        ESP_LOGE(TAG, "STA has no IP (not connected to internet)");
        return;
    }

    ESP_LOGI(TAG, "STA IP: " IPSTR " (internet connection)", IP2STR(&sta_ip_info.ip));
    ESP_LOGI(TAG, "STA Gateway: " IPSTR, IP2STR(&sta_ip_info.gw));
    ESP_LOGI(TAG, "AP IP: " IPSTR " (hotspot)", IP2STR(&ap_ip_info.ip));

    // Step 7: Configure DNS forwarder
    // Get DNS server from STA interface (or use 8.8.8.8 as fallback)
    esp_netif_dns_info_t dns_info;
    ip_addr_t dnsserver;
    
    if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK && 
        dns_info.ip.u_addr.ip4.addr != 0)
    {
        // Use router's DNS
        dnsserver.u_addr.ip4.addr = dns_info.ip.u_addr.ip4.addr;
        ESP_LOGI(TAG, "Using router DNS: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
    }
    else
    {
        // Fallback to Google DNS (8.8.8.8)
        dnsserver.u_addr.ip4.addr = htonl(0x08080808);  // 8.8.8.8 in network byte order
        ESP_LOGI(TAG, "Using fallback DNS: 8.8.8.8");
    }
    dnsserver.type = IPADDR_TYPE_V4;
    
    // Store DNS for the forwarder task
    upstream_dns.type = IPADDR_TYPE_V4;
    upstream_dns.u_addr.ip4.addr = dnsserver.u_addr.ip4.addr;
    
    // Step 8: Enable NAT (Network Address Translation) for internet sharing
    // NAT translates packets between the AP network (192.168.4.x) and the internet
    // This is the KEY to making internet sharing work!
    //
    // Important: NAT must be enabled on the AP address (192.168.4.1), NOT the STA
    if (!napt_enabled || napt_address != ap_addr)
    {
        // Disable old NAT if it was enabled on a different address
        if (napt_enabled && napt_address != 0)
        {
            ESP_LOGI(TAG, "Disabling old NAT on 0x%08lx", (unsigned long)napt_address);
            ip_napt_enable(napt_address, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // Enable NAT on the AP address (192.168.4.1)
        // The AP address is already in network byte order from esp_netif_get_ip_info
        ESP_LOGI(TAG, "Enabling NAT on AP address: 192.168.4.1");
        ip_napt_enable(ap_addr, 1);
        
        napt_enabled = true;
        napt_address = ap_addr;
        
        ESP_LOGI(TAG, "NAT enabled successfully!");
        ESP_LOGI(TAG, "Internet routing: Clients(192.168.4.x) -> ESP32(192.168.4.1) -> Router -> Internet");
    }
    else
    {
        ESP_LOGI(TAG, "NAT already enabled");
    }
    
    // Step 9: Mark hotspot as enabled
    hotspot_enabled = true;
    
    // Step 10: Start DNS forwarder task for automatic DNS resolution
    if (dns_forwarder_task_handle == NULL)
    {
        xTaskCreate(dns_forwarder_task, "dns_forwarder", 3072, NULL, 5, &dns_forwarder_task_handle);
        ESP_LOGI(TAG, "DNS forwarder started");
    }
    
    ESP_LOGI(TAG, "Hotspot enabled successfully");
    ESP_LOGI(TAG, "SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "Password: %s", ap_config.ap.authmode == WIFI_AUTH_OPEN ? "None (Open)" : "********");
    ESP_LOGI(TAG, "IP Address: 192.168.4.1");
    ESP_LOGI(TAG, "DNS: Automatic (forwarded to " IPSTR ")", IP2STR((ip4_addr_t*)&upstream_dns.u_addr.ip4.addr));
    ESP_LOGI(TAG, "NAT: Enabled (full internet sharing)");
}

// ============================================================================
// disable_hotspot()
// ============================================================================
// Disables the hotspot and cleans up all resources.
// This stops the DNS forwarder, disables NAT, and switches back to STA-only mode.
// ============================================================================
void disable_hotspot(void)
{
    if (!hotspot_enabled)
    {
        ESP_LOGI(TAG, "Hotspot already disabled");
        return;
    }

    ESP_LOGI(TAG, "Disabling hotspot...");

    // Step 1: Stop DNS forwarder
    // Setting hotspot_enabled=false will cause the DNS forwarder loop to exit
    hotspot_enabled = false;
    
    if (dns_forwarder_task_handle != NULL)
    {
        ESP_LOGI(TAG, "Stopping DNS forwarder");
        vTaskDelay(pdMS_TO_TICKS(200));  // Give task time to exit cleanly
        
        // Close socket if still open
        if (dns_forwarder_socket >= 0)
        {
            close(dns_forwarder_socket);
            dns_forwarder_socket = -1;
        }
        
        dns_forwarder_task_handle = NULL;
        ESP_LOGI(TAG, "DNS forwarder stopped");
    }

    // Step 2: Disable NAT
    if (napt_enabled && napt_address != 0)
    {
        ESP_LOGI(TAG, "Disabling NAT");
        ip_napt_enable(napt_address, 0);
        napt_enabled = false;
        napt_address = 0;
    }

    // Step 3: Switch WiFi back to Station-only mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Hotspot disabled successfully");
}

bool is_hotspot_enabled(void)
{
    return hotspot_enabled;
}


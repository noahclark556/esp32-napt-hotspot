/***************************************************************************************
 *  File        : napt_interface.h
 *  Description : ESP32 WiFi hotspot with internet sharing via NAPT
 *  Author      : Noah Clark
 *  Created     : 2026-02-01
 *  Version     : 1.0.0
 *--------------------------------------------------------------------------------------
 *  See README.md for complete setup instructions.
 *--------------------------------------------------------------------------------------
 ***************************************************************************************/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enable WiFi hotspot with internet sharing
 * 
 * Creates a WiFi access point with full internet sharing via NAPT.
 * 
 * Prerequisites:
 * - ESP32 must be connected to WiFi in STA mode first
 * - WiFi must be initialized (esp_wifi_init() called)
 * - Event loop must be running
 * 
 * Network topology after enabling:
 * [Internet] <-> [Router] <-> [ESP32 STA] <-> [ESP32 AP: 192.168.4.1] <-> [Clients]
 * 
 * @param ssid WiFi network name (SSID) for the hotspot. Pass NULL to use default.
 * @param password WiFi password (must be at least 8 characters). Pass NULL to use default.
 *                 If password is less than 8 characters, an open network is created.
 * 
 * @note This function will:
 *       1. Create an AP interface with IP 192.168.4.1
 *       2. Switch WiFi to APSTA mode (STA + AP simultaneously)
 *       3. Enable NAPT for internet sharing
 *       4. Start DNS forwarder for automatic DNS resolution
 * 
 * @note Clients connecting to this hotspot will automatically receive:
 *       - IP address: 192.168.4.x (via DHCP)
 *       - Gateway: 192.168.4.1 (the ESP32)
 *       - DNS: 192.168.4.1 (forwarded to upstream DNS)
 *       - Full internet access via NAPT
 */
void enable_hotspot(const char *ssid, const char *password);

/**
 * @brief Disable WiFi hotspot
 * 
 * Stops the hotspot, disables NAPT, stops DNS forwarding, and switches
 * WiFi back to STA-only mode.
 * 
 * This function will:
 * 1. Stop DNS forwarder task
 * 2. Disable NAPT (internet sharing)
 * 3. Switch WiFi back to STA mode
 * 
 * @note This does not disconnect from the router (STA connection remains active)
 */
void disable_hotspot(void);

/**
 * @brief Check if hotspot is currently enabled
 * 
 * @return true if hotspot is enabled, false otherwise
 */
bool is_hotspot_enabled(void);

#ifdef __cplusplus
}
#endif
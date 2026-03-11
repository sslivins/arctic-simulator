/*
 * WiFi Manager for Arctic Simulator
 *
 * Supports two modes:
 *   1. Station (STA) — connects to a known network using credentials
 *      stored in NVS (set via the captive portal or menuconfig).
 *   2. Provisioning (SoftAP) — creates an open access point with a
 *      captive portal so the user can pick a network and enter a
 *      password from any phone or laptop browser.
 *
 * Boot flow:
 *   - If NVS has stored credentials → try STA.
 *   - If STA fails or no creds → start SoftAP provisioning.
 *   - On successful provisioning → save creds to NVS, reboot.
 *   - A long button press (>3 s) can reset stored creds.
 */
#pragma once

#include "esp_err.h"

namespace wifi {

/// Current WiFi operating mode.
enum class Mode {
    IDLE,           // Not started yet
    CONNECTING,     // Attempting STA connection
    CONNECTED,      // STA connected, IP acquired
    PROVISIONING,   // SoftAP captive portal active
};

/// Initialize WiFi subsystem:
///   1. Check NVS for stored SSID/password.
///   2. If found, attempt STA connection (blocking, with timeout).
///   3. If STA fails or no creds exist, start SoftAP provisioning.
/// Returns ESP_OK when STA connected, ESP_ERR_NOT_FINISHED when
/// provisioning is active (caller should start the API server anyway).
esp_err_t init();

/// Current mode.
Mode getMode();

/// True when STA is connected and has an IP.
bool isConnected();

/// STA IP address as string ("0.0.0.0" when not connected).
const char* getIPAddress();

/// SoftAP SSID (only meaningful when provisioning).
const char* getAPName();

/// Erase stored WiFi credentials from NVS.  The caller should reboot
/// afterward so the device re-enters provisioning mode.
esp_err_t eraseCredentials();

}  // namespace wifi

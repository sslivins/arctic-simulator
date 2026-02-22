/*
 * WiFi Manager for Arctic Simulator
 * Connects to a configured WiFi network in station mode.
 */
#pragma once

#include "esp_err.h"

namespace wifi {

// Initialize WiFi and connect to the configured network
// Blocks until connected or fails after retries
esp_err_t init();

// Check if connected
bool isConnected();

// Get IP address as string (returns "0.0.0.0" if not connected)
const char* getIPAddress();

}  // namespace wifi

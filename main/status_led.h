/*
 * Status LED driver for M5Stack Atom S3 / S3R
 *
 * Drives the SK6812 RGB LED on the top of the device.
 * Green = WiFi connected and healthy.
 * Red   = not connected or error state.
 */
#pragma once

#include "esp_err.h"

namespace status_led {

/// Initialise the RMT-based LED strip driver.
esp_err_t init();

/// Set the LED to solid green (healthy).
void setGreen();

/// Set the LED to solid red (unhealthy / not connected).
void setRed();

/// Turn the LED off.
void off();

}  // namespace status_led

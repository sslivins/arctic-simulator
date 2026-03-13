/*
 * Display driver for M5Stack Atom S3 / S3R
 *
 * Drives the 128×128 GC9107 LCD via SPI and provides a simple
 * status UI.  Also handles front-button polling with debounce.
 */
#pragma once

#include "esp_err.h"

namespace display {

/// Initialise LCD, backlight, button GPIO.
/// Safe to call on boards without a display — returns an error
/// but does not crash.
esp_err_t init();

/// Redraw the status screen (call periodically, e.g. every 500 ms).
void refresh();

/// Poll the front button.  Returns true once per press (debounced).
bool checkButton();

}  // namespace display

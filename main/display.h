/*
 * LCD Display Driver for M5Stack Atom S3 / S3R
 *
 * Drives the 0.85" 128×128 GC9107 LCD via SPI using the esp_lcd
 * ST7789-compatible panel driver.  Renders a simple status UI with
 * Modbus stats, WiFi state, and a recording indicator.  The front-face
 * button (GPIO 41) toggles local recording on/off.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

namespace display {

// Initialize SPI bus, LCD panel, backlight, and framebuffer.
esp_err_t init();

// Redraw the full UI (call at ~2 Hz from a task).
void refresh();

// Poll the front-face button (call at ~20 Hz).
// Returns true on a debounced press edge.
bool checkButton();

}  // namespace display

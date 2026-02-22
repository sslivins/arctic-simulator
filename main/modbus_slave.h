/*
 * Modbus RTU Slave for Arctic Simulator
 * Responds to master (controller) requests using esp_modbus slave API.
 */
#pragma once

#include "esp_err.h"

namespace mb_slave {

// Initialize Modbus RTU slave on RS-485
esp_err_t init();

// Deinitialize
void deinit();

// Check if initialized
bool isInitialized();

// Process pending Modbus events (call from task loop)
// Returns true if a register was read or written by the master
bool processEvents();

// Statistics
struct Stats {
    uint32_t read_count;    // Total read requests served
    uint32_t write_count;   // Total write requests received
    uint32_t error_count;   // Errors
};

Stats getStats();
void resetStats();

}  // namespace mb_slave

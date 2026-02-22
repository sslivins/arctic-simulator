/*
 * REST API Server for Arctic Simulator
 * Provides HTTP endpoints to control simulated register values.
 */
#pragma once

#include "esp_err.h"

namespace api {

// Start HTTP server
esp_err_t start();

// Stop HTTP server
void stop();

// Check if running
bool isRunning();

}  // namespace api

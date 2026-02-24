/*
 * Arctic Heat Pump Simulator
 *
 * Emulates an ECO-600 heat pump's Modbus RTU slave interface
 * on an M5Stack Atom S3 with RS-485 adapter.
 *
 * Modes:
 *   - Interactive: REST API sets register values, Modbus serves them
 *   - Playback: Loads a JSONL capture file, replays register states
 *
 * Access the API at http://arctic-sim.local/api/
 */
#include "register_map.h"
#include "modbus_slave.h"
#include "api_server.h"
#include "playback.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

// ============================================================================
// Modbus processing task
// ============================================================================

static void modbusTask(void* param) {
    ESP_LOGI(TAG, "Modbus task started");
    while (true) {
        mb_slave::processEvents();
        // Small yield — the slave blocks internally on event wait
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ============================================================================
// Playback tick task
// ============================================================================

static void playbackTask(void* param) {
    ESP_LOGI(TAG, "Playback task started");
    while (true) {
        playback::tick();
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms resolution
    }
}

// ============================================================================
// Entry point
// ============================================================================

extern "C" void app_main(void) {
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGI(TAG, "=== Arctic Heat Pump Simulator v%s ===", app->version);

    // Initialize NVS (required for WiFi)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Initialize register map with idle defaults
    reg::init();

    // Initialize playback engine
    playback::init();

    // Initialize Modbus slave
    err = mb_slave::init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus init failed — continuing without Modbus");
    } else {
        // Start Modbus processing task
        xTaskCreatePinnedToCore(modbusTask, "modbus", 4096, nullptr, 5, nullptr, 1);
    }

    // Connect to WiFi
    err = wifi::init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi failed — API will not be available");
    } else {
        // Start HTTP API server
        api::start();
        ESP_LOGI(TAG, "API available at http://%s/api/status", wifi::getIPAddress());
    }

    // Start playback task
    xTaskCreatePinnedToCore(playbackTask, "playback", 4096, nullptr, 3, nullptr, 0);

    ESP_LOGI(TAG, "Simulator ready");
    ESP_LOGI(TAG, "  Modbus: %s (slave addr %d, 2400 8E1)",
             mb_slave::isInitialized() ? "active" : "inactive",
             CONFIG_SIMULATOR_MODBUS_SLAVE_ADDR);
    ESP_LOGI(TAG, "  WiFi:   %s", wifi::isConnected() ? wifi::getIPAddress() : "disconnected");
    ESP_LOGI(TAG, "  API:    %s", api::isRunning() ? "running" : "stopped");
}

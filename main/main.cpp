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
#include "display.h"
#include "recorder.h"

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
// Display refresh + button polling task
// ============================================================================

static void displayTask(void* param) {
    ESP_LOGI(TAG, "Display task started");
    int count = 0;
    while (true) {
        // Poll button every 50ms
        if (display::checkButton()) {
            // Toggle recording on button press (only in normal mode)
            if (wifi::getMode() != wifi::Mode::PROVISIONING) {
                if (recorder::isRecording()) {
                    recorder::stop();
                    ESP_LOGI(TAG, "Recording stopped via button");
                } else {
                    recorder::start();
                    ESP_LOGI(TAG, "Recording started via button");
                }
            }
        }
        // Refresh screen every 500ms (every 10th iteration)
        if (++count >= 10) {
            display::refresh();
            count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ============================================================================
// Recorder tick task
// ============================================================================

static void recorderTask(void* param) {
    ESP_LOGI(TAG, "Recorder task started");
    while (true) {
        recorder::tick();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SIMULATOR_RECORD_INTERVAL_MS));
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

    // Initialize recorder (SPIFFS)
    recorder::init();

    // Initialize display (LCD + button)
    display::init();

    // Initialize Modbus slave
    err = mb_slave::init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus init failed — continuing without Modbus");
    } else {
        // Start Modbus processing task
        xTaskCreatePinnedToCore(modbusTask, "modbus", 4096, nullptr, 5, nullptr, 1);
    }

    // Connect to WiFi (or start provisioning portal)
    err = wifi::init();
    if (err == ESP_OK) {
        // STA connected — start the main API server
        api::start();
        ESP_LOGI(TAG, "API available at http://%s/api/status", wifi::getIPAddress());
    } else if (err == ESP_ERR_NOT_FINISHED) {
        // Provisioning mode — captive portal is already running
        ESP_LOGI(TAG, "WiFi provisioning active — connect to '%s'", wifi::getAPName());
    } else {
        ESP_LOGE(TAG, "WiFi failed — API will not be available");
    }

    // Start playback task
    xTaskCreatePinnedToCore(playbackTask, "playback", 4096, nullptr, 3, nullptr, 0);

    // Start display task
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 2, nullptr, 0);

    // Start recorder task
    xTaskCreatePinnedToCore(recorderTask, "recorder", 4096, nullptr, 2, nullptr, 0);

    ESP_LOGI(TAG, "Simulator ready");
    ESP_LOGI(TAG, "  Modbus: %s (slave addr %d, 2400 8E1)",
             mb_slave::isInitialized() ? "active" : "inactive",
             CONFIG_SIMULATOR_MODBUS_SLAVE_ADDR);
    if (wifi::getMode() == wifi::Mode::PROVISIONING) {
        ESP_LOGI(TAG, "  WiFi:   provisioning (AP: %s)", wifi::getAPName());
    } else {
        ESP_LOGI(TAG, "  WiFi:   %s", wifi::isConnected() ? wifi::getIPAddress() : "disconnected");
    }
    ESP_LOGI(TAG, "  API:    %s", api::isRunning() ? "running" : "stopped");
}

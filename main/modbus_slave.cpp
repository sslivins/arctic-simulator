/*
 * Modbus RTU Slave — Implementation
 * Uses freemodbus via esp_modbus component in slave mode.
 */
#include "modbus_slave.h"
#include "register_map.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "mbcontroller.h"
#include <string.h>

static const char* TAG = "mb_slave";

namespace mb_slave {

static bool s_initialized = false;
static Stats s_stats = {};

esp_err_t init() {
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing Modbus RTU slave (addr=%d, %d baud, 8E1)",
             CONFIG_SIMULATOR_MODBUS_SLAVE_ADDR, 2400);

    // Initialize slave controller
    void* slave_handle = nullptr;
    esp_err_t err = mbc_slave_init(MB_PORT_SERIAL_SLAVE, &slave_handle);
    if (err != ESP_OK || slave_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to init slave: %s", esp_err_to_name(err));
        return err;
    }

    // Configure communication
    mb_communication_info_t comm_info = {};
    comm_info.port = (uart_port_t)CONFIG_SIMULATOR_UART_PORT;
    comm_info.mode = MB_MODE_RTU;
    comm_info.slave_addr = (uint8_t)CONFIG_SIMULATOR_MODBUS_SLAVE_ADDR;
    comm_info.baudrate = 2400;
    comm_info.parity = UART_PARITY_EVEN;

    err = mbc_slave_setup((void*)&comm_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup slave: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    // Register holding registers area (2000–2057)
    // The Modbus slave will read/write directly into our register_map arrays
    mb_register_area_descriptor_t holding_area = {};
    holding_area.type = MB_PARAM_HOLDING;
    holding_area.start_offset = reg::HOLDING_BASE;
    holding_area.address = (void*)reg::holdingData();
    holding_area.size = reg::HOLDING_COUNT * sizeof(uint16_t);

    err = mbc_slave_set_descriptor(holding_area);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set holding descriptor: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    // Register input registers area (2100–2138)
    // We map these as holding registers too since the protocol uses FC 0x03
    // (read holding registers) for both ranges
    mb_register_area_descriptor_t input_area = {};
    input_area.type = MB_PARAM_HOLDING;
    input_area.start_offset = reg::INPUT_BASE;
    input_area.address = (void*)reg::inputData();
    input_area.size = reg::INPUT_COUNT * sizeof(uint16_t);

    err = mbc_slave_set_descriptor(input_area);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set input descriptor: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    // Start the slave
    err = mbc_slave_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start slave: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    // Configure UART pins after start (driver is now installed)
    err = uart_set_pin((uart_port_t)CONFIG_SIMULATOR_UART_PORT,
                       CONFIG_SIMULATOR_RS485_TX_PIN,
                       CONFIG_SIMULATOR_RS485_RX_PIN,
                       CONFIG_SIMULATOR_RS485_DIR_PIN,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    // Set RS-485 half-duplex mode
    err = uart_set_mode((uart_port_t)CONFIG_SIMULATOR_UART_PORT,
                        UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set RS-485 mode: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    s_initialized = true;
    resetStats();

    ESP_LOGI(TAG, "Modbus slave started (TX:%d, RX:%d, DIR:%d)",
             CONFIG_SIMULATOR_RS485_TX_PIN, CONFIG_SIMULATOR_RS485_RX_PIN,
             CONFIG_SIMULATOR_RS485_DIR_PIN);

    return ESP_OK;
}

void deinit() {
    if (!s_initialized) return;
    mbc_slave_destroy();
    s_initialized = false;
    ESP_LOGI(TAG, "Modbus slave stopped");
}

bool isInitialized() {
    return s_initialized;
}

bool processEvents() {
    if (!s_initialized) return false;

    // Check for Modbus events (non-blocking with short timeout)
    mb_event_group_t event = mbc_slave_check_event(
        (mb_event_group_t)(MB_EVENT_HOLDING_REG_WR | MB_EVENT_HOLDING_REG_RD));

    if (event & MB_EVENT_HOLDING_REG_RD) {
        s_stats.read_count++;
        return true;
    }
    if (event & MB_EVENT_HOLDING_REG_WR) {
        s_stats.write_count++;
        ESP_LOGD(TAG, "Master wrote register(s)");
        return true;
    }

    return false;
}

Stats getStats() { return s_stats; }
void resetStats() { memset(&s_stats, 0, sizeof(s_stats)); }

}  // namespace mb_slave

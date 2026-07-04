/*
 * Arctic (Macon) Heat Pump Register Map — Implementation
 *
 * The Tuya slave serves bytes out of tuya_state, so every mutation here is
 * mirrored into tuya_state (projectSet) to reach the wire. The uint16_t
 * arrays remain the convenient source for the REST API / display reads.
 */
#include "register_map.h"
#include "tuya_state.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "reg";

namespace reg {

// Internal storage
static uint16_t s_holding[HOLDING_COUNT] = {};
static uint16_t s_input[INPUT_COUNT]     = {};

// Static 7-byte prefix that precedes the telemetry-window register data on
// the real Macon unit (observed constant in every capture). The controller
// may validate it, so seed it so our frames are byte-identical to the OEM.
static const uint8_t TELEMETRY_PREFIX[7] = { 0x0a, 0x28, 0x32, 0x05, 0x01, 0x00, 0x0f };

// ============================================================================
// Helpers
// ============================================================================

bool isHolding(uint16_t addr) {
    return addr >= HOLDING_BASE && addr <= HOLDING_END;
}

bool isInput(uint16_t addr) {
    return addr >= INPUT_BASE && addr <= INPUT_END;
}

bool isValid(uint16_t addr) {
    return isHolding(addr) || isInput(addr);
}

uint16_t* holdingData() { return s_holding; }
uint16_t* inputData()   { return s_input;   }

uint16_t get(uint16_t addr) {
    if (isHolding(addr)) return s_holding[addr - HOLDING_BASE];
    if (isInput(addr))   return s_input[addr - INPUT_BASE];
    return 0;
}

esp_err_t set(uint16_t addr, uint16_t value) {
    if (isHolding(addr)) {
        s_holding[addr - HOLDING_BASE] = value;
        tuya_state::projectSet(addr, value);   // mirror onto the wire
        return ESP_OK;
    }
    if (isInput(addr)) {
        s_input[addr - INPUT_BASE] = value;
        tuya_state::projectSet(addr, value);   // mirror onto the wire
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

// Project the entire register_map into tuya_state (used after bulk preset
// loads that write the arrays directly). Also (re)seeds the telemetry prefix.
static void syncAllToTuya() {
    for (uint8_t i = 0; i < sizeof(TELEMETRY_PREFIX); ++i) {
        tuya_state::setByte(/*field_a=*/0, i, TELEMETRY_PREFIX[i]);
    }
    for (uint16_t a = HOLDING_BASE; a <= HOLDING_END; ++a) {
        tuya_state::projectSet(a, s_holding[a - HOLDING_BASE]);
    }
    for (uint16_t a = INPUT_BASE; a <= INPUT_END; ++a) {
        tuya_state::projectSet(a, s_input[a - INPUT_BASE]);
    }
}

// ============================================================================
// Presets
// ============================================================================

static void clearAll() {
    memset(s_holding, 0, sizeof(s_holding));
    memset(s_input, 0, sizeof(s_input));
}

void clearErrors() {
    s_input[FAULT - INPUT_BASE] = 0;
    tuya_state::projectSet(FAULT, 0);
}

// Signed-byte encoding helper for negative temperatures (-6 °C -> 250).
static inline uint16_t tempByte(int v) {
    return (uint16_t)(uint8_t)(int8_t)v;
}

static void setCommonDefaults() {
    // Mains present even when idle. AC voltage is x10 on the wire (raw 23 =
    // 230 V); DC bus is x10 (raw ~36 = 360 V when running, low when idle).
    s_input[AC_VOLTAGE - INPUT_BASE]        = 23;   // 230 V
    s_holding[HOT_WATER_SETPOINT - HOLDING_BASE] = 55;
}

void loadPreset(Preset preset) {
    clearAll();
    setCommonDefaults();

    switch (preset) {
    case Preset::IDLE:
        ESP_LOGI(TAG, "Loading preset: IDLE");
        // Compressor/pump off (status byte 0), ambient temps set.
        s_input[STATUS_BYTE - INPUT_BASE]          = 0;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(20);
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = tempByte(25);
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = tempByte(25);
        s_holding[WATER_TANK_TEMP - HOLDING_BASE]  = tempByte(45);
        break;

    case Preset::HEATING:
        ESP_LOGI(TAG, "Loading preset: HEATING");
        s_input[STATUS_BYTE - INPUT_BASE]          = STS_COMPRESSOR | STS_WATER_PUMP;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 50;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(5);
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = tempByte(35);
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = tempByte(42);
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = tempByte(75);
        s_input[SUCTION_TEMP - INPUT_BASE]         = tempByte(3);
        s_input[COIL_TEMP - INPUT_BASE]            = tempByte(2);
        s_input[IPM_TEMP - INPUT_BASE]             = tempByte(45);
        s_input[MAIN_EEV - INPUT_BASE]             = 200;
        s_input[REALTIME_POWER - INPUT_BASE]       = 28;   // ~2800 W
        s_holding[AC_CURRENT - HOLDING_BASE]       = 12;   // 12 A
        s_holding[DC_BUS_VOLTAGE - HOLDING_BASE]   = 36;   // 360 V
        s_holding[DC_MOTOR_SPEED - HOLDING_BASE]   = 70;
        break;

    case Preset::COOLING:
        ESP_LOGI(TAG, "Loading preset: COOLING");
        s_input[STATUS_BYTE - INPUT_BASE]          = STS_COMPRESSOR | STS_WATER_PUMP;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 60;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(35);
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = tempByte(12);
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = tempByte(8);
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = tempByte(65);
        s_input[SUCTION_TEMP - INPUT_BASE]         = tempByte(5);
        s_input[COOL_COIL_TEMP - INPUT_BASE]       = tempByte(6);
        s_input[IPM_TEMP - INPUT_BASE]             = tempByte(50);
        s_input[MAIN_EEV - INPUT_BASE]             = 250;
        s_input[REALTIME_POWER - INPUT_BASE]       = 30;   // ~3000 W
        s_holding[AC_CURRENT - HOLDING_BASE]       = 13;
        s_holding[DC_BUS_VOLTAGE - HOLDING_BASE]   = 36;
        s_holding[DC_MOTOR_SPEED - HOLDING_BASE]   = 80;
        break;

    case Preset::HOT_WATER:
        ESP_LOGI(TAG, "Loading preset: HOT_WATER");
        s_input[STATUS_BYTE - INPUT_BASE]          = STS_COMPRESSOR | STS_WATER_PUMP;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 55;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(20);
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = tempByte(40);
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = tempByte(48);
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = tempByte(85);
        s_input[SUCTION_TEMP - INPUT_BASE]         = tempByte(8);
        s_input[IPM_TEMP - INPUT_BASE]             = tempByte(48);
        s_input[REALTIME_POWER - INPUT_BASE]       = 32;   // ~3200 W
        s_holding[WATER_TANK_TEMP - HOLDING_BASE]  = tempByte(42);
        s_holding[HOT_WATER_SETPOINT - HOLDING_BASE] = 55;
        s_holding[AC_CURRENT - HOLDING_BASE]       = 12;
        s_holding[DC_BUS_VOLTAGE - HOLDING_BASE]   = 36;
        break;

    case Preset::DEFROST:
        ESP_LOGI(TAG, "Loading preset: DEFROST");
        s_input[STATUS_BYTE - INPUT_BASE]          = STS_COMPRESSOR | STS_WATER_PUMP;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 40;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(-2);
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = tempByte(30);
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = tempByte(28);
        s_input[COIL_TEMP - INPUT_BASE]            = tempByte(-5);
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = tempByte(50);
        s_holding[DC_MOTOR_SPEED - HOLDING_BASE]   = 0;    // fan off during defrost
        break;

    case Preset::FAULT_P01:
        ESP_LOGI(TAG, "Loading preset: FAULT_P01 (water-flow protection)");
        // Compressor tripped off, water-flow fault bit set (reg 2128 bit7).
        s_input[STATUS_BYTE - INPUT_BASE]          = 0;
        s_input[FAULT - INPUT_BASE]                = FAULT_P01_WATER_FLOW;  // 0x80
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = tempByte(20);
        break;
    }

    syncAllToTuya();
}

void init() {
    // The Tuya slave serves from tuya_state; make sure it exists before we
    // project the initial preset into it.
    tuya_state::init();
    loadPreset(Preset::IDLE);
    ESP_LOGI(TAG, "Register map initialized (holding: %d regs, input: %d regs)",
             HOLDING_COUNT, INPUT_COUNT);
}

}  // namespace reg

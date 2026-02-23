/*
 * Arctic Heat Pump Register Map — Implementation
 */
#include "register_map.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "reg";

namespace reg {

// Internal storage
static uint16_t s_holding[HOLDING_COUNT] = {};
static uint16_t s_input[INPUT_COUNT]     = {};

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
        return ESP_OK;
    }
    if (isInput(addr)) {
        s_input[addr - INPUT_BASE] = value;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

// ============================================================================
// Presets
// ============================================================================

static void clearAll() {
    memset(s_holding, 0, sizeof(s_holding));
    memset(s_input, 0, sizeof(s_input));
}

void clearErrors() {
    s_input[ERROR_CODE_1 - INPUT_BASE] = 0;
    s_input[ERROR_CODE_2 - INPUT_BASE] = 0;
    s_input[ERROR_CODE_3 - INPUT_BASE] = 0;
}

static void setCommonDefaults() {
    // Typical setpoints (whole °C, no scaling per protocol)
    s_holding[COOLING_SETPOINT - HOLDING_BASE]    = 7;
    s_holding[HEATING_SETPOINT - HOLDING_BASE]    = 45;
    s_holding[HOT_WATER_SETPOINT - HOLDING_BASE]  = 55;
    s_holding[COOLING_DELTA_T - HOLDING_BASE]     = 5;
    s_holding[HEATING_DELTA_T - HOLDING_BASE]     = 5;
    s_holding[HOT_WATER_DELTA_T - HOLDING_BASE]   = 5;
    s_holding[FAN_COIL_HEATING_DT - HOLDING_BASE] = 5;

    // Common sensor readings
    s_input[AC_VOLTAGE - INPUT_BASE]   = 230;   // 230V
    s_input[DC_VOLTAGE - INPUT_BASE]   = 3200;  // protocol says ÷10 → 320V
    s_input[EE_CODING - INPUT_BASE]    = 1;
}

void loadPreset(Preset preset) {
    clearAll();
    setCommonDefaults();

    switch (preset) {
    case Preset::IDLE:
        ESP_LOGI(TAG, "Loading preset: IDLE");
        // Unit off, ambient temps set, everything else zero
        s_holding[UNIT_ON_OFF - HOLDING_BASE] = 0;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 20;
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = 25;
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = 25;
        s_input[WATER_TANK_TEMP - INPUT_BASE]      = 45;
        break;

    case Preset::HEATING:
        ESP_LOGI(TAG, "Loading preset: HEATING");
        s_holding[UNIT_ON_OFF - HOLDING_BASE]    = 1;
        s_holding[WORKING_MODE - HOLDING_BASE]   = MODE_FLOOR_HEATING;
        s_holding[HEATING_SETPOINT - HOLDING_BASE] = 45;

        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 5;
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = 35;
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = 42;
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = 75;
        s_input[SUCTION_TEMP - INPUT_BASE]         = 3;
        s_input[OUTDOOR_COIL_TEMP - INPUT_BASE]    = 2;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 55;
        s_input[FAN_SPEED - INPUT_BASE]            = 600;
        s_input[AC_CURRENT - INPUT_BASE]           = 8;
        s_input[PRIMARY_EEV - INPUT_BASE]          = 200;
        s_input[HIGH_PRESSURE - INPUT_BASE]        = 250;  // ÷100 → 2.50 MPa
        s_input[LOW_PRESSURE - INPUT_BASE]         = 80;   // ÷100 → 0.80 MPa
        s_input[IPM_TEMP - INPUT_BASE]             = 45;

        // Status: unit on, compressor on, fan low, water pump on, water flow OK
        s_input[STATUS_2 - INPUT_BASE] = STS2_UNIT_ON | STS2_COMPRESSOR |
                                         STS2_FAN_LOW | STS2_WATER_PUMP |
                                         STS2_WATER_FLOW;
        break;

    case Preset::COOLING:
        ESP_LOGI(TAG, "Loading preset: COOLING");
        s_holding[UNIT_ON_OFF - HOLDING_BASE]    = 1;
        s_holding[WORKING_MODE - HOLDING_BASE]   = MODE_COOLING;
        s_holding[COOLING_SETPOINT - HOLDING_BASE] = 7;

        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 35;
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = 12;
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = 8;
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = 65;
        s_input[SUCTION_TEMP - INPUT_BASE]         = 5;
        s_input[OUTDOOR_COIL_TEMP - INPUT_BASE]    = 50;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 60;
        s_input[FAN_SPEED - INPUT_BASE]            = 700;
        s_input[AC_CURRENT - INPUT_BASE]           = 10;
        s_input[PRIMARY_EEV - INPUT_BASE]          = 250;
        s_input[HIGH_PRESSURE - INPUT_BASE]        = 300;  // ÷100 → 3.00 MPa
        s_input[LOW_PRESSURE - INPUT_BASE]         = 60;   // ÷100 → 0.60 MPa
        s_input[IPM_TEMP - INPUT_BASE]             = 50;

        s_input[STATUS_2 - INPUT_BASE] = STS2_UNIT_ON | STS2_COMPRESSOR |
                                         STS2_FAN_MED | STS2_WATER_PUMP |
                                         STS2_WATER_FLOW | STS2_4WAY_VALVE;
        break;

    case Preset::HOT_WATER:
        ESP_LOGI(TAG, "Loading preset: HOT_WATER");
        s_holding[UNIT_ON_OFF - HOLDING_BASE]    = 1;
        s_holding[WORKING_MODE - HOLDING_BASE]   = MODE_HOT_WATER;
        s_holding[HOT_WATER_SETPOINT - HOLDING_BASE] = 55;

        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 20;
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = 40;
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = 48;
        s_input[WATER_TANK_TEMP - INPUT_BASE]      = 42;
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = 85;
        s_input[SUCTION_TEMP - INPUT_BASE]         = 8;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 70;
        s_input[FAN_SPEED - INPUT_BASE]            = 500;
        s_input[AC_CURRENT - INPUT_BASE]           = 12;
        s_input[HIGH_PRESSURE - INPUT_BASE]        = 350;  // ÷100 → 3.50 MPa
        s_input[LOW_PRESSURE - INPUT_BASE]         = 90;   // ÷100 → 0.90 MPa

        s_input[STATUS_2 - INPUT_BASE] = STS2_UNIT_ON | STS2_COMPRESSOR |
                                         STS2_FAN_LOW | STS2_WATER_PUMP |
                                         STS2_WATER_FLOW | STS2_3WAY_V1;
        break;

    case Preset::DEFROST:
        ESP_LOGI(TAG, "Loading preset: DEFROST");
        s_holding[UNIT_ON_OFF - HOLDING_BASE]    = 1;
        s_holding[WORKING_MODE - HOLDING_BASE]   = MODE_FLOOR_HEATING;

        // Negative temps: encoding TBD (need real captures to confirm)
        // Using two's complement uint16 for now: -2 → 65534, -5 → 65531
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = (uint16_t)(-2);  // -2°C
        s_input[INLET_WATER_TEMP - INPUT_BASE]     = 30;
        s_input[OUTLET_WATER_TEMP - INPUT_BASE]    = 28;
        s_input[OUTDOOR_COIL_TEMP - INPUT_BASE]    = (uint16_t)(-5);  // -5°C
        s_input[DISCHARGE_TEMP - INPUT_BASE]       = 50;
        s_input[COMPRESSOR_FREQ - INPUT_BASE]      = 40;
        s_input[FAN_SPEED - INPUT_BASE]            = 0;     // Fan off during defrost

        // Status: defrost active, 4-way valve reversed
        s_input[STATUS_2 - INPUT_BASE] = STS2_UNIT_ON | STS2_COMPRESSOR |
                                         STS2_WATER_PUMP | STS2_4WAY_VALVE;
        s_input[STATUS_3 - INPUT_BASE] = (1 << 5);  // Defrost bit
        break;

    case Preset::ERROR_E01:
        ESP_LOGI(TAG, "Loading preset: ERROR_E01 (discharge temp sensor)");
        s_holding[UNIT_ON_OFF - HOLDING_BASE] = 0;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 20;
        // E01 = discharge temp sensor error = register 2137 bit 6
        s_input[ERROR_CODE_2 - INPUT_BASE] = (1 << 6);
        break;

    case Preset::ERROR_P01:
        ESP_LOGI(TAG, "Loading preset: ERROR_P01 (water flow protection)");
        s_holding[UNIT_ON_OFF - HOLDING_BASE] = 0;
        s_input[OUTDOOR_AMBIENT_TEMP - INPUT_BASE] = 20;
        // P01 = water flow switch protection = register 2138 bit 8
        s_input[ERROR_CODE_3 - INPUT_BASE] = (1 << 8);
        break;
    }
}

void init() {
    loadPreset(Preset::IDLE);
    ESP_LOGI(TAG, "Register map initialized (holding: %d regs, input: %d regs)",
             HOLDING_COUNT, INPUT_COUNT);
}

}  // namespace reg

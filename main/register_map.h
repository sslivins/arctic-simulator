/*
 * Arctic Heat Pump Register Map
 * Mirrors the ECO-600 register layout for simulation.
 * Based on EVI DC Inverter Heat Pump Communication Protocol V1.3
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace reg {

// ============================================================================
// Register address ranges
// ============================================================================
constexpr uint16_t HOLDING_BASE  = 2000;
constexpr uint16_t HOLDING_COUNT = 58;   // 2000-2057
constexpr uint16_t HOLDING_END   = HOLDING_BASE + HOLDING_COUNT - 1;

constexpr uint16_t INPUT_BASE    = 2100;
constexpr uint16_t INPUT_COUNT   = 39;   // 2100-2138
constexpr uint16_t INPUT_END     = INPUT_BASE + INPUT_COUNT - 1;

// ============================================================================
// Holding Register Addresses (R/W) — 2000–2057
// ============================================================================
constexpr uint16_t UNIT_ON_OFF           = 2000;  // 0=OFF, 1=ON
constexpr uint16_t WORKING_MODE          = 2001;  // 0=Cool, 1=Floor heat, 2=Fan coil heat, 5=Hot water, 6=Auto
constexpr uint16_t COOLING_SETPOINT      = 2002;
constexpr uint16_t HEATING_SETPOINT      = 2003;
constexpr uint16_t HOT_WATER_SETPOINT    = 2004;
constexpr uint16_t COOLING_DELTA_T       = 2005;
constexpr uint16_t HEATING_DELTA_T       = 2006;
constexpr uint16_t HOT_WATER_DELTA_T     = 2007;
constexpr uint16_t FAN_COIL_HEATING_DT   = 2008;

// P1–P47 technician parameters
constexpr uint16_t P1_EEV_OPENING        = 2009;
constexpr uint16_t P5_STERILIZE_TIME     = 2013;
constexpr uint16_t P13_MAX_TEMP          = 2021;
constexpr uint16_t P23_COOL_AUTO_TEMP    = 2031;
constexpr uint16_t P24_HEAT_AUTO_TEMP    = 2032;
constexpr uint16_t P28_MODE_SWITCH_DELAY = 2036;
constexpr uint16_t P29_DEFROST_CYCLE     = 2037;
constexpr uint16_t P30_DEFROST_ENTER     = 2038;
constexpr uint16_t P34_MAX_DEFROST_TIME  = 2042;
constexpr uint16_t P35_DEFROST_EXIT      = 2043;
constexpr uint16_t P41_EEV_SUPERHEAT     = 2049;
constexpr uint16_t P44_PUMP_MODE         = 2052;
constexpr uint16_t P47_WATERWAY_CLEAN    = 2055;
constexpr uint16_t FREQ_CTRL_ENABLE      = 2056;
constexpr uint16_t FREQ_CTRL_SETTING     = 2057;

// ============================================================================
// Input Register Addresses (R/O) — 2100–2138
// ============================================================================

// Temperatures (whole °C, UINT16 — negative encoding TBD, see protocol doc)
constexpr uint16_t WATER_TANK_TEMP       = 2100;
constexpr uint16_t OUTLET_WATER_TEMP     = 2102;
constexpr uint16_t INLET_WATER_TEMP      = 2103;
constexpr uint16_t DISCHARGE_TEMP        = 2104;
constexpr uint16_t SUCTION_TEMP          = 2105;
constexpr uint16_t EVI_SUCTION_TEMP      = 2106;
constexpr uint16_t OUTDOOR_COIL_TEMP     = 2107;
constexpr uint16_t INDOOR_COIL_TEMP      = 2108;
constexpr uint16_t INDOOR_AMBIENT_TEMP   = 2109;
constexpr uint16_t OUTDOOR_AMBIENT_TEMP  = 2110;
constexpr uint16_t HP_SAT_TEMP           = 2111;
constexpr uint16_t LP_SAT_TEMP           = 2112;
constexpr uint16_t EVI_LP_SAT_TEMP       = 2113;
constexpr uint16_t IPM_TEMP              = 2114;
constexpr uint16_t BRINE_INLET_TEMP      = 2115;
constexpr uint16_t BRINE_OUTLET_TEMP     = 2116;

// Electrical / mechanical
constexpr uint16_t COMPRESSOR_FREQ       = 2118;
constexpr uint16_t FAN_SPEED             = 2119;
constexpr uint16_t AC_VOLTAGE            = 2120;
constexpr uint16_t AC_CURRENT            = 2121;
constexpr uint16_t DC_VOLTAGE            = 2122;  // ÷10 for actual V
constexpr uint16_t COMP_PHASE_CURRENT    = 2123;
constexpr uint16_t PRIMARY_EEV           = 2124;
constexpr uint16_t SECONDARY_EEV         = 2125;
constexpr uint16_t HIGH_PRESSURE         = 2126;  // ÷100 for MPa
constexpr uint16_t LOW_PRESSURE          = 2127;  // ÷100 for MPa
constexpr uint16_t EE_CODING             = 2128;

// Status / error registers (bit fields)
constexpr uint16_t STATUS_1              = 2133;  // Frequency limit flags
constexpr uint16_t ERROR_CODE_1          = 2134;  // Brine/tank sensor errors
constexpr uint16_t STATUS_2              = 2135;  // System working status
constexpr uint16_t STATUS_3              = 2136;  // Extended status
constexpr uint16_t ERROR_CODE_2          = 2137;  // Sensor/communication errors
constexpr uint16_t ERROR_CODE_3          = 2138;  // Protection errors

// ============================================================================
// Enums
// ============================================================================
enum WorkingMode : uint16_t {
    MODE_COOLING         = 0,
    MODE_FLOOR_HEATING   = 1,
    MODE_FAN_COIL_HEAT   = 2,
    MODE_HOT_WATER       = 5,
    MODE_AUTO            = 6,
};

// Status register 2135 bit definitions
enum Status2Bits : uint16_t {
    STS2_UNIT_ON         = (1 << 0),
    STS2_COMPRESSOR      = (1 << 1),
    STS2_FAN_HIGH        = (1 << 2),
    STS2_FAN_MED         = (1 << 3),
    STS2_FAN_LOW         = (1 << 4),
    STS2_WATER_PUMP      = (1 << 5),
    STS2_4WAY_VALVE      = (1 << 6),
    STS2_ELEC_HEATER     = (1 << 7),
    STS2_WATER_FLOW      = (1 << 8),
    STS2_HP_SWITCH       = (1 << 9),
    STS2_LP_SWITCH       = (1 << 10),
    STS2_REMOTE_ON       = (1 << 11),
    STS2_MODE_SWITCH     = (1 << 12),
    STS2_3WAY_V1         = (1 << 13),
    STS2_3WAY_V2         = (1 << 14),
    STS2_BRINE_FLOW      = (1 << 15),
};

// ============================================================================
// Preset names
// ============================================================================
enum class Preset {
    IDLE,
    HEATING,
    COOLING,
    HOT_WATER,
    DEFROST,
    ERROR_E01,
    ERROR_P01,
};

// ============================================================================
// API
// ============================================================================

// Initialize register map with idle defaults
void init();

// Get/set individual registers by Modbus address
// Returns ESP_ERR_NOT_FOUND for invalid addresses
uint16_t get(uint16_t addr);
esp_err_t set(uint16_t addr, uint16_t value);

// Check if address is valid
bool isHolding(uint16_t addr);
bool isInput(uint16_t addr);
bool isValid(uint16_t addr);

// Bulk access — returns pointer to internal array for Modbus slave binding
uint16_t* holdingData();
uint16_t* inputData();

// Load a preset state
void loadPreset(Preset preset);

// Clear all error flags
void clearErrors();

}  // namespace reg

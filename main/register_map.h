/*
 * Arctic (Macon) Heat Pump Register Map
 *
 * Address layout for the REAL Arctic heat pump (OEM = Macon) as observed on
 * the wire and reverse-engineered in arctic-controller applyMaconMapping().
 * This replaces the earlier ECO-600 numbering — the Macon device reuses the
 * same wire register numbers for entirely different fields.
 *
 * Wire format is 1 byte/reg (see arctic-sniffer docs/TUYA-ARCTIC-PROTOCOL.md).
 * Storage here is uint16_t for convenience; values are masked to a byte by
 * tuya_state when projected into the slave's window store, so anything > 255
 * in a preset silently truncates to its low byte.
 *
 * Temperatures are whole °C signed bytes (e.g. -6 °C -> raw 250). Electrical
 * scales (applied only for display, elsewhere): AC/DC voltage x10, real-time
 * power x100, current x1.
 *
 * IMPORTANT: the Tuya slave serves bytes from tuya_state, NOT from this
 * module's arrays. Every write here is mirrored into tuya_state so the wire
 * reflects it (see register_map.cpp).
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
constexpr uint16_t INPUT_COUNT   = 43;   // 2100-2142 (telemetry window reaches 2142)
constexpr uint16_t INPUT_END     = INPUT_BASE + INPUT_COUNT - 1;

// ============================================================================
// "Holding" window (wire addr=50, base 2000) — telemetry on the Macon unit
// ============================================================================
constexpr uint16_t AC_CURRENT            = 2000;  // A4  AC input current (A)
constexpr uint16_t DC_BUS_VOLTAGE        = 2001;  // A7  DC bus voltage (x10 = V)
constexpr uint16_t DC_MOTOR_SPEED        = 2003;  // A10 DC (fan) motor speed
constexpr uint16_t WATER_TANK_TEMP       = 2008;  // o1  water tank temp (°C)
constexpr uint16_t HOT_WATER_SETPOINT    = 2012;  // hot water setpoint (°C)

// ============================================================================
// "Telemetry" window (wire addr=0, base 2100 after a 7-byte prefix)
// ============================================================================
constexpr uint16_t AC_VOLTAGE            = 2101;  // A13 AC input voltage (x10 = V)
constexpr uint16_t MAIN_EEV              = 2104;  // A5  main elec. expansion valve (steps)
constexpr uint16_t IPM_TEMP              = 2113;  // A8  IPM module temp (°C)
constexpr uint16_t REALTIME_POWER        = 2114;  // A9  real-time power (x100 = W)
constexpr uint16_t FAULT                 = 2128;  // fault/protection bitfield
constexpr uint16_t RUNNING_FLAG          = 2129;  // running flag (tentative)
constexpr uint16_t STATUS_BYTE           = 2130;  // status: bit2=comp, bit3=pump
constexpr uint16_t OUTLET_WATER_TEMP     = 2132;  // o3  outlet (supply) water temp (°C)
constexpr uint16_t INLET_WATER_TEMP      = 2133;  // o2  inlet (return) water temp (°C)
constexpr uint16_t OUTDOOR_AMBIENT_TEMP  = 2134;  // o4  ambient temp (°C)
constexpr uint16_t COOL_COIL_TEMP        = 2135;  // A6  cool coil temp (°C)
constexpr uint16_t SUCTION_TEMP          = 2136;  // A3  suction temp (°C)
constexpr uint16_t COIL_TEMP             = 2137;  // A2  coil temp (°C)
constexpr uint16_t DISCHARGE_TEMP        = 2138;  // A1  discharge temp (°C)
constexpr uint16_t COMPRESSOR_FREQ       = 2141;  // A14 compressor frequency (Hz)

// ============================================================================
// Status byte (reg 2130) bit definitions — active outputs.
// Confirmed live: bit2 = compressor running, bit3 = water pump running.
// Other bits observed but not yet decoded.
// ============================================================================
enum StatusBits : uint16_t {
    STS_COMPRESSOR  = (1 << 2),  // 0x04
    STS_WATER_PUMP  = (1 << 3),  // 0x08
};

// ============================================================================
// Fault byte (reg 2128) bit definitions — Macon protection/fault bitfield.
// Only bit7 confirmed live (P01 water-flow). Macon bit ordering does NOT
// match the legacy Arctic error tables, so other bits are left undecoded.
// ============================================================================
enum FaultBits : uint16_t {
    FAULT_P01_WATER_FLOW = (1 << 7),  // 0x80
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
    FAULT_P01,
};

// ============================================================================
// API
// ============================================================================

// Initialize register map with idle defaults (also seeds the tuya_state
// telemetry-window prefix and projects the initial state onto the wire).
void init();

// Get/set individual registers by wire address.
// Returns ESP_ERR_NOT_FOUND for invalid addresses. set() mirrors the value
// into tuya_state so the Tuya slave serves it on the wire.
uint16_t get(uint16_t addr);
esp_err_t set(uint16_t addr, uint16_t value);

// Check if address is valid
bool isHolding(uint16_t addr);
bool isInput(uint16_t addr);
bool isValid(uint16_t addr);

// Bulk access — returns pointer to internal array.
uint16_t* holdingData();
uint16_t* inputData();

// Load a preset state (mirrors the whole map into tuya_state).
void loadPreset(Preset preset);

// Clear the fault register.
void clearErrors();

}  // namespace reg

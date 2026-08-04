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

constexpr uint16_t INPUT_BASE    = 2093;
constexpr uint16_t INPUT_COUNT   = 50;   // 2093-2142 (full telemetry window; byte0 = reg2093)
constexpr uint16_t INPUT_END     = INPUT_BASE + INPUT_COUNT - 1;

// ============================================================================
// "Holding" window (wire addr=50, base 2000) — telemetry on the Macon unit
// ============================================================================
constexpr uint16_t AC_CURRENT            = 2000;  // A4  AC input current (A)
constexpr uint16_t DC_BUS_VOLTAGE        = 2001;  // A7  DC bus voltage (x10 = V)
constexpr uint16_t DC_MOTOR_SPEED        = 2003;  // A10 DC (fan) motor speed
constexpr uint16_t RUN_STATE             = 2007;  // operating-state code (see RunState)
constexpr uint16_t WATER_TANK_TEMP       = 2008;  // o1  water tank temp (°C)
constexpr uint16_t HOT_WATER_SETPOINT    = 2012;  // hot water setpoint (°C)

// ============================================================================
// "Telemetry" window (wire addr=0, base 2093; byte0 = reg2093)
// ----------------------------------------------------------------------------
// Regs 2093..2099 were formerly treated as an opaque 7-byte "prefix"; the
// arctic-macon codec (311a291) resolved them to real registers. reg2093 is the
// cooling setpoint (confirmed live: flips the instant the controller dial moves
// 12->24 C). 2094..2099 are still mostly unconfirmed but seeded to the values
// observed constant on the OEM unit so wire output stays byte-identical.
// ============================================================================
constexpr uint16_t COOLING_SETPOINT      = 2093;  // cooling setpoint (whole °C)
constexpr uint16_t AC_VOLTAGE            = 2101;  // A13 AC input voltage (x10 = V)
constexpr uint16_t MAIN_EEV              = 2104;  // A5  main elec. expansion valve (steps)
constexpr uint16_t IPM_TEMP              = 2113;  // A8  IPM module temp (°C)
constexpr uint16_t REALTIME_POWER        = 2114;  // A9  real-time power (x100 = W)
constexpr uint16_t FAULT_SENSOR_EE       = 2125;  // fault bitfield: sensor/EE/comm E-codes
constexpr uint16_t FAULT_SENSOR_COMP     = 2126;  // fault bitfield: sensor/comm/compressor (E + r01/r02)
constexpr uint16_t FAULT_ELEC            = 2127;  // fault bitfield: electrical/power-stage (r-codes + P02/P11)
constexpr uint16_t FAULT                 = 2128;  // fault bitfield: refrigerant/protection (P-codes)
constexpr uint16_t ICON_BITS2            = 2129;  // icon bitfield #2 (see IconBits2)
constexpr uint16_t STATUS_BYTE           = 2130;  // icon bitfield #1 (see StatusBits)
constexpr uint16_t OUTLET_WATER_TEMP     = 2132;  // o3  outlet (supply) water temp (°C)
constexpr uint16_t INLET_WATER_TEMP      = 2133;  // o2  inlet (return) water temp (°C)
constexpr uint16_t OUTDOOR_AMBIENT_TEMP  = 2134;  // o4  ambient temp (°C)
constexpr uint16_t COOL_COIL_TEMP        = 2135;  // A6  cool coil temp (°C)
constexpr uint16_t SUCTION_TEMP          = 2136;  // A3  suction temp (°C)
constexpr uint16_t COIL_TEMP             = 2137;  // A2  coil temp (°C)
constexpr uint16_t DISCHARGE_TEMP        = 2138;  // A1  discharge temp (°C)
constexpr uint16_t COMPRESSOR_FREQ       = 2141;  // A14 compressor frequency (Hz)

// ============================================================================
// Operating-state / fault register (reg 2007, HOLDING) — THE register the OEM
// controller reads to decide whether to show the ON / hot-water icon. Isolated
// live 2026-07-04, then FULLY MAPPED 2026-07-05: it is an 8-bit BITFIELD (not an
// enum). bit5 (0x20) = the hot-water RUN indicator; bits0-3 = differential /
// temp-diff faults; bits 4/6/7 unused.
//   bit0 0x01 = P15 (inlet/outlet ΔT too large)
//   bit1 0x02 = P16 (outlet water temp too low)
//   bit2 0x04 = FE  (start differential-pressure protection; app shows offline)
//   bit3 0x08 = FF  (run differential protection; app shows offline)
//   bit5 0x20 = hot-water RUNNING (ON) indicator
// NOTE: reg 2130 (STATUS_BYTE below) is NOT what the controller reads for the ON
// icon — the real unit runs ON with 2130 = 0. Only bit5 (0x20) is the confirmed
// running code on this DHW controller; other running modes (heating/cooling)
// reuse it as the best-known running code.
// ============================================================================
enum RunState : uint16_t {
    RUN_OFF        = 0,
    RUN_FAULT_P15  = (1 << 0),  // 0x01
    RUN_FAULT_P16  = (1 << 1),  // 0x02
    RUN_FAULT_FE   = (1 << 2),  // 0x04
    RUN_FAULT_FF   = (1 << 3),  // 0x08
    RUN_HOT_WATER  = (1 << 5),  // 0x20  hot-water running (ON)
};

// ============================================================================
// Icon/status bitfield (reg 2130) — drives per-icon segments on the OEM display.
// FULLY MAPPED live on the OEM controller 2026-07-04 (one bit at a time):
//   bit0 0x01 = heating icon (flashes when compressor bit not set)
//   bit1 0x02 = (unused/reserved — no visible effect)
//   bit2 0x04 = compressor icon
//   bit3 0x08 = water pump icon
//   bit4 0x10 = (unused/reserved — no visible effect)
//   bit5 0x20 = "hours" icon (next to setpoint)
//   bit6 0x40 = parameter/diagnostic A-code scroll mode
//   bit7 0x80 = LCD segment test (lights every icon) — DO NOT set in presets
// NOTE: this is NOT the ON-icon driver (see RUN_STATE above). The real unit runs
// hot-water ON with 2130 = 0x0C (compressor + pump). Bits 6/7 are service modes.
// ============================================================================
enum StatusBits : uint16_t {
    STS_HEATING     = (1 << 0),  // 0x01  heating icon
    STS_COMPRESSOR  = (1 << 2),  // 0x04
    STS_WATER_PUMP  = (1 << 3),  // 0x08
    STS_HOURS       = (1 << 5),  // 0x20  "hours" icon
    STS_DIAG_SCROLL = (1 << 6),  // 0x40  service: A-code scroll (do not set normally)
    STS_LCD_TEST    = (1 << 7),  // 0x80  service: all-segment LCD test (do not set normally)
};

// ============================================================================
// Icon bitfield #2 (reg 2129) — second per-icon segment field on the OEM display.
// Mapped live 2026-07-04 (reg2129=0xFF lit only fan + defrost; other bits inert):
//   bit1 0x02 = defrost icon
//   bit4 0x10 = fan icon
// NOTE: the OEM fan icon comes from THIS bit, not reg2003 (which is fan SPEED).
// ============================================================================
enum IconBits2 : uint16_t {
    ICO2_DEFROST = (1 << 1),  // 0x02  defrost icon
    ICO2_FAN     = (1 << 4),  // 0x10  fan icon
};
// ============================================================================
// Fault bitfields (INPUT window) — four 8-bit protection/fault registers, all
// mapped live 2026-07-05 one bit at a time against the OEM LCD + Smart Life app
// and cross-referenced to the official Arctic fault catalog. When multiple bits
// are set the OEM controller CYCLES through the active codes. "PT" in app text =
// "Protection". The input fault cluster is exactly reg2125-2128 (reg2124=timer
// mode, reg2131=A-code diag scroll — NOT faults).
// ============================================================================
enum FaultRefrigBits : uint16_t {   // reg2128 — refrigerant / P-codes
    FAULT_P06_LOW_PRESS    = (1 << 0),  // 0x01
    FAULT_P27_COIL_OVERHEAT= (1 << 1),  // 0x02
    FAULT_PC_AMBIENT       = (1 << 2),  // 0x04
    FAULT_P10              = (1 << 3),  // 0x08 (device code, not in manual)
    FAULT_P30_ANTIFREEZE   = (1 << 4),  // 0x10
    FAULT_E05_COIL_SENSOR  = (1 << 5),  // 0x20
    FAULT_P01_WATER_FLOW   = (1 << 7),  // 0x80
};

enum FaultElecBits : uint16_t {     // reg2127 — electrical / r-codes + P02/P11
    FAULT_P19_AC_CURRENT   = (1 << 1),  // 0x02
    FAULT_R06_COMP_PHASE   = (1 << 2),  // 0x04
    FAULT_R10_AC_VOLTAGE   = (1 << 3),  // 0x08
    FAULT_R11_DC_BUS       = (1 << 4),  // 0x10
    FAULT_R05_IPM_TEMP     = (1 << 5),  // 0x20
    FAULT_P11_HIGH_DISCH   = (1 << 6),  // 0x40
    FAULT_P02_HIGH_PRESS   = (1 << 7),  // 0x80
};

enum FaultSensorCompBits : uint16_t {  // reg2126 — sensor/comm/compressor
    FAULT_R02_COMP_START   = (1 << 0),  // 0x01
    FAULT_E26_INOUT_COMM   = (1 << 1),  // 0x02
    FAULT_R01_IPM          = (1 << 2),  // 0x04
    FAULT_E01_DISCHARGE_SNS= (1 << 4),  // 0x10
    FAULT_E09_SUCTION_SNS  = (1 << 5),  // 0x20
    FAULT_E05_COIL_SNS     = (1 << 6),  // 0x40
    FAULT_E22_AMBIENT_SNS  = (1 << 7),  // 0x80
};

enum FaultSensorEeBits : uint16_t {    // reg2125 — sensor/EE/comm E-codes
    FAULT_E28_OUTDOOR_EE   = (1 << 0),  // 0x01
    FAULT_E19_INLET_SNS    = (1 << 1),  // 0x02
    FAULT_E18_OUTLET_SNS   = (1 << 2),  // 0x04
    FAULT_E13_COOLCOIL_SNS = (1 << 3),  // 0x08
    FAULT_E03              = (1 << 4),  // 0x10 (device code, not in manual)
    FAULT_E28_INDOOR_EE    = (1 << 5),  // 0x20
    FAULT_E27_DRIVER_COMM  = (1 << 6),  // 0x40
    FAULT_E21_CTRL_COMM    = (1 << 7),  // 0x80
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

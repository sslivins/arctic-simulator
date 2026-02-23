# Arctic Heat Pump Modbus Communication Protocol

> Based on: EVI DC Inverter Heat Pump Communication Protocol V1.3

## Overview

This document describes the Modbus RTU communication protocol for Arctic EVI DC Inverter Heat Pumps.

- **Master/Slave**: Master = upper computer (controller); Slave = heat pump communication board
- **Slave Address**: 1 (fixed)

---

## Table of Contents

1. [Communication Settings](#communication-settings)
2. [Protocol Overview](#protocol-overview)
3. [Function Codes](#function-codes)
4. [Communication Timing](#communication-timing)
5. [Register Map](#register-map)
6. [Status Codes](#status-codes)
7. [Error Codes](#error-codes)

---

## Communication Settings

| Parameter | Value |
|-----------|-------|
| Communication Method | Half-duplex asynchronous serial (RS-485) |
| Baud Rate | 2400 BPS |
| Data Bits | 8 bits, LSB first |
| Parity | Even |
| Start Bit | 1 bit (low level) |
| Stop Bit | 1 bit (high level) |

### Data Transmission Format (per byte)

```
┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
│ ST │ D0 │ D1 │ D2 │ D3 │ D4 │ D5 │ D6 │ D7 │ PT │ SP │
└────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘
```

- **ST**: Start Bit
- **D0-D7**: Data Bits (8 bits, LSB first)
- **PT**: Parity Bit (Even parity)
- **SP**: Stop Bit

---

## Protocol Overview

### Frame Format

| Field | Size | Description |
|-------|------|-------------|
| Device Address | 8 bits | Slave address (1-255, this device uses **1**) |
| Function Code | 8 bits | Read/write function identifier |
| Data | N × 8 bits | Communication data |
| CRC Check | 16 bits | CRC-16 error check (low byte first) |

---

## Function Codes

### Function Code 0x03: Read Holding Registers

Read multiple 16-bit registers from the slave.

**Master Request:**

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x03 |
| Register Start Address (High) | Start register high byte |
| Register Start Address (Low) | Start register low byte |
| Register Count (High) | Number of registers high byte |
| Register Count (Low) | Number of registers low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

**Slave Response:**

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x03 |
| Data Length | Number of data bytes (N × 2) |
| Data 1 (High) | Register 1 high byte |
| Data 1 (Low) | Register 1 low byte |
| ... | ... |
| Data N (High) | Register N high byte |
| Data N (Low) | Register N low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

---

### Function Code 0x06: Write Single Register

Write a single 16-bit register to the slave.

**Master Request:**

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x06 |
| Register Address (High) | Register address high byte |
| Register Address (Low) | Register address low byte |
| Data (High) | Value high byte |
| Data (Low) | Value low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

**Slave Response:** (Echo of request)

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x06 |
| Register Address (High) | Register address high byte |
| Register Address (Low) | Register address low byte |
| Data (High) | Value high byte |
| Data (Low) | Value low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

---

### Function Code 0x10 (16): Write Multiple Registers *(Not Recommended)*

Write multiple 16-bit registers to the slave.

**Master Request:**

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x10 (16) |
| Register Start Address (High) | Start register high byte |
| Register Start Address (Low) | Start register low byte |
| Register Count (High) | Number of registers high byte |
| Register Count (Low) | Number of registers low byte |
| Data Length | Number of data bytes |
| Data 1 (High) | Register 1 high byte |
| Data 1 (Low) | Register 1 low byte |
| ... | ... |
| Data N (High) | Register N high byte |
| Data N (Low) | Register N low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

**Slave Response:**

| Field | Description |
|-------|-------------|
| Device Address | Slave address |
| Function Code | 0x10 (16) |
| Register Start Address (High) | Start register high byte |
| Register Start Address (Low) | Start register low byte |
| Register Count (High) | Number of registers high byte |
| Register Count (Low) | Number of registers low byte |
| CRC (Low) | CRC low byte |
| CRC (High) | CRC high byte |

---

## Communication Timing

| Parameter | Value |
|-----------|-------|
| Polling Interval | 500 ms (master sends request every 500ms) |
| Response Delay | 50 ms (slave waits before responding) |
| Response Timeout | 200 ms (master resends if no response) |
| Communication Error | 10 s (slave considers comm error if no master signal) |

### Normal Communication

```
Remote Module    ┌──┐              ┌──┐              ┌──┐
(Master)      ───┘  └──────────────┘  └──────────────┘  └───
                 │<─── 500ms ────>│
                 │
                 │ 50ms
                 │<──>│
Heat Pump     ───────┐  ┌─────────────┐  ┌─────────────┐  ┌─
(Slave)              └──┘             └──┘             └──┘
```

The master sends a request every 500ms. The slave responds after a 50ms delay.

### Abnormal Communication (Retry on Timeout)

```
Remote Module    ┌──┐    ┌──┐         ┌──┐    ┌──┐         ┌──┐
(Master)      ───┘  └────┘  └─────────┘  └────┘  └─────────┘  └───
                 │<─────>│<── 500ms ──>│<─────>│
                 │ 200ms │             │ 200ms │
                 │       │             │       │
Heat Pump     ───────────────────────────────────────────────┐  ┌─
(Slave)                    (no response - timeout)           └──┘
```

If the master does not receive a response within 200ms, it resends the request. After multiple retries, normal communication resumes when the slave responds.

---

## Register Map

### Holding Registers (Read/Write) - Addresses 2000-2099

| Address | Function Code | Description | Data Type | Range | Notes |
|---------|---------------|-------------|-----------|-------|-------|
| 2000 | 03/06/16 | Unit ON/OFF setting | UINT16 | 0-1 | 0=OFF, 1=ON |
| 2001 | 03/06/16 | Working mode setting | UINT16 | 0,1,2,5,6 | 0=Cooling, 1=Floor heating, 2=Fan coil heating, 5=Hot water, 6=Auto (3,4 unused) |
| 2002 | 03/06/16 | Cooling temperature setting | UINT16 | | |
| 2003 | 03/06/16 | Heating temperature setting | UINT16 | | |
| 2004 | 03/06/16 | Hot water temperature setting | UINT16 | | |
| 2005 | 03/06/16 | Fan coil cooling ΔT | UINT16 | | |
| 2006 | 03/06/16 | Underfloor heating ΔT | UINT16 | | |
| 2007 | 03/06/16 | Hot water tank ΔT | UINT16 | | |
| 2008 | 03/06/16 | Fan coil heating ΔT | UINT16 | | |
| 2009 | 03/06/16 | (P1) Main EEV initial opening setting | UINT16 | 0-500 | |
| 2010 | 03/06/16 | (P2) | UINT16 | | |
| 2011 | 03/06/16 | (P3) | UINT16 | | |
| 2012 | 03/06/16 | (P4) | UINT16 | | |
| 2013 | 03/06/16 | (P5) Sterilizing time setting | UINT16 | | |
| 2014 | 03/06/16 | (P6) | UINT16 | | |
| 2015 | 03/06/16 | (P7) | UINT16 | | |
| 2016 | 03/06/16 | (P8) | UINT16 | | |
| 2017 | 03/06/16 | (P9) | UINT16 | | |
| 2018 | 03/06/16 | (P10) | UINT16 | | |
| 2019 | 03/06/16 | (P11) | UINT16 | | |
| 2020 | 03/06/16 | (P12) | UINT16 | | |
| 2021 | 03/06/16 | (P13) Maximum setting temperature | UINT16 | | |
| 2022 | 03/06/16 | (P14) | UINT16 | | |
| 2023 | 03/06/16 | (P15) | UINT16 | | |
| 2024 | 03/06/16 | (P16) | UINT16 | | |
| 2025 | 03/06/16 | (P17) | UINT16 | | |
| 2026 | 03/06/16 | (P18) | UINT16 | | |
| 2027 | 03/06/16 | (P19) | UINT16 | | |
| 2028 | 03/06/16 | (P20) | UINT16 | | |
| 2029 | 03/06/16 | (P21) | UINT16 | | |
| 2030 | 03/06/16 | (P22) | UINT16 | | |
| 2031 | 03/06/16 | (P23) Cooling ambient temp setting for auto mode | UINT16 | | |
| 2032 | 03/06/16 | (P24) Heating ambient temp setting for auto mode | UINT16 | | |
| 2033 | 03/06/16 | (P25) | UINT16 | | |
| 2034 | 03/06/16 | (P26) | UINT16 | | |
| 2035 | 03/06/16 | (P27) | UINT16 | | |
| 2036 | 03/06/16 | (P28) Mode switch delay under auto mode | UINT16 | | |
| 2037 | 03/06/16 | (P29) Defrost cycle | UINT16 | | |
| 2038 | 03/06/16 | (P30) Coil temp to enter defrost mode | UINT16 | | |
| 2039 | 03/06/16 | (P31) Ambient temp to extend defrost time | UINT16 | | |
| 2040 | 03/06/16 | (P32) Ambient-coil temp diff to enter defrost | UINT16 | | |
| 2041 | 03/06/16 | (P33) Extend defrost cycle time setting | UINT16 | | |
| 2042 | 03/06/16 | (P34) Maximum defrost time | UINT16 | | |
| 2043 | 03/06/16 | (P35) Coil temp to exit defrost mode | UINT16 | | |
| 2044 | 03/06/16 | (P36) Water return cycle temp setting | UINT16 | | |
| 2045 | 03/06/16 | (P37) Water return cycle time setting | UINT16 | | |
| 2046 | 03/06/16 | (P38) Low ambient temp protection setting | UINT16 | | |
| 2047 | 03/06/16 | (P39) Freq reduction near target temp | UINT16 | | |
| 2048 | 03/06/16 | (P40) Cooling low ambient temp protection | UINT16 | | |
| 2049 | 03/06/16 | (P41) Main EEV superheat control selection | UINT16 | | 0=Superheat adjustment, 1=Fixed-point adjustment |
| 2050 | 03/06/16 | (P42) Main EEV target superheat degree | UINT16 | | |
| 2051 | 03/06/16 | (P43) 3-way valve 2 switching time | UINT16 | | |
| 2052 | 03/06/16 | (P44) Water pump mode after reaching target temp | UINT16 | | 0=ON/OFF per P45, 1=Keep OFF, 2=Keep ON |
| 2053 | 03/06/16 | (P45) Water pump running interval | UINT16 | | |
| 2054 | 03/06/16 | (P46) Low ambient temp to turn on pump in standby | UINT16 | | |
| 2055 | 03/06/16 | (P47) Waterway cleaning function selection | UINT16 | | 0=OFF, 1=Pump ON, 2=Pump+3WV1, 3=Pump+3WV1+3WV2 |
| 2056 | 03/06/16 | Accept frequency control from host unit | UINT16 | 0-1 | 0=NO, 1=YES |
| 2057 | 03/06/16 | Host unit compressor frequency setting | UINT16 | 0-120 | |
| 2058-2099 | | Reserved | | | |

### Input Registers (Read-Only) - Addresses 2100-2138

#### Temperature & Sensor Registers (2100-2132)

| Address | Description | Data Type | Notes |
|---------|-------------|-----------|-------|
| 2100 | Water tank temperature | UINT16 | |
| 2101 | Reserved | UINT16 | |
| 2102 | Outlet water temperature | UINT16 | |
| 2103 | Inlet water temperature | UINT16 | |
| 2104 | Discharge temperature | UINT16 | |
| 2105 | Suction temperature | UINT16 | |
| 2106 | Reserved (EVI suction temperature) | UINT16 | |
| 2107 | External coil temperature | UINT16 | |
| 2108 | Cooling coil temperature | UINT16 | |
| 2109 | Reserved (Indoor temperature) | UINT16 | |
| 2110 | Outdoor ambient temperature | UINT16 | |
| 2111 | Reserved (High pressure saturation temp) | UINT16 | |
| 2112 | Reserved (Primary circuit low pressure saturation temp) | UINT16 | |
| 2113 | Reserved (Secondary low pressure saturation temp) | UINT16 | |
| 2114 | IPM temperature | UINT16 | |
| 2115 | Brine side inlet water temperature | UINT16 | |
| 2116 | Brine side outlet water temperature | UINT16 | |
| 2117 | Reserved temperature 3 | UINT16 | |
| 2118 | Compressor running frequency | UINT16 | |
| 2119 | DC fan motor speed | UINT16 | |
| 2120 | AC voltage | UINT16 | V |
| 2121 | AC current | UINT16 | A |
| 2122 | DC voltage | UINT16 | ÷10 for actual V |
| 2123 | Compressor phase current | UINT16 | A |
| 2124 | Primary EEV opening | UINT16 | steps |
| 2125 | Secondary EEV opening | UINT16 | steps |
| 2126 | High pressure | UINT16 | ÷100 for MPa |
| 2127 | Low pressure | UINT16 | ÷100 for MPa |
| 2128 | EE coding | UINT16 | |
| 2129 | Reserved | UINT16 | |
| 2130 | Reserved | UINT16 | |
| 2131 | Reserved | UINT16 | |
| 2132 | Reserved | UINT16 | |

#### Status Register 2133 - System Working Status 1

| Bit | Description |
|-----|-------------|
| 0 | Frequency reaches upper limit |
| 1 | Frequency reaches lower limit |
| 2-15 | Reserved |

#### Status Register 2134 - Error Code 1

| Bit | Description |
|-----|-------------|
| 0 | Brine side inlet water temperature sensor error |
| 1 | Brine side outlet water temperature sensor error |
| 2 | Brine side water flow protection |
| 3 | Water tank temperature sensor error |
| 4-15 | Reserved |

#### Status Register 2135 - System Working Status 2

| Bit | Description |
|-----|-------------|
| 0 | Unit ON/OFF status |
| 1 | Compressor status |
| 2 | High wind speed (fan motor output) |
| 3 | Medium wind speed |
| 4 | Low wind speed |
| 5 | Water pump |
| 6 | 4-way valve |
| 7 | Electric heater |
| 8 | Water flow switch |
| 9 | High pressure switch |
| 10 | Low pressure switch |
| 11 | Remote ON/OFF switch for all modes |
| 12 | Mode switch |
| 13 | 3-way valve 1 |
| 14 | 3-way valve 2 |
| 15 | Brine side water flow switch |

#### Status Register 2136 - System Working Status 3

| Bit | Description |
|-----|-------------|
| 0 | Solenoid valve |
| 1 | Unloading valve |
| 2 | Oil return valve |
| 3 | Brine side water pump |
| 4 | Brine side antifreeze |
| 5 | Defrost |
| 6 | Refrigerant recovery |
| 7 | Oil return |
| 8 | Wired controller connecting status |
| 9 | Energy-saving operation |
| 10 | Primary antifreeze protection |
| 11 | Secondary antifreeze protection |
| 12 | High temperature sterilizing |
| 13 | Secondary water pump |
| 14 | Remote ON/OFF switch for heating/cooling mode |
| 15 | Reserved |

#### Status Register 2137 - Error Code 2

| Bit | Description |
|-----|-------------|
| 0 | Indoor EE error |
| 1 | Outdoor EE error |
| 2 | Inlet water temp sensor error |
| 3 | Outlet water temp sensor error |
| 4 | Cooling coil antifreeze protection |
| 5 | External coil temp sensor error |
| 6 | Discharge temperature sensor error |
| 7 | Suction temperature sensor error |
| 8 | Ambient temperature sensor error |
| 9 | Communication error between drive board and main board |
| 10 | Wired controller communication error |
| 11 | Compressor abnormal start |
| 12 | Communication error between indoor and outdoor unit |
| 13 | IPM error |
| 14 | High outlet water temperature protection |
| 15 | AC voltage protection |

#### Status Register 2138 - Error Code 3

| Bit | Description |
|-----|-------------|
| 0 | AC current protection |
| 1 | Compressor current protection |
| 2 | DC fan motor protection |
| 3 | Bus voltage protection |
| 4 | IPM temperature protection |
| 5 | High discharge temperature protection |
| 6 | High pressure switch protection |
| 7 | Low pressure switch protection |
| 8 | Water flow switch protection |
| 9 | Cooling external coil overheat protection |
| 10 | Low ambient temperature protection |
| 11 | Primary circuit low pressure protection |
| 12 | Secondary circuit low pressure protection |
| 13 | Large inlet/outlet temperature difference protection |
| 14 | Low outlet water temperature protection |
| 15 | Compressor running differential |

---

## Quick Reference

### Key Registers for Basic Operation

| Address | Description | Read/Write |
|---------|-------------|------------|
| 2000 | Unit ON/OFF | R/W |
| 2001 | Working Mode | R/W |
| 2002 | Cooling Setpoint | R/W |
| 2003 | Heating Setpoint | R/W |
| 2004 | Hot Water Setpoint | R/W |
| 2102 | Outlet Water Temp | R |
| 2103 | Inlet Water Temp | R |
| 2110 | Outdoor Ambient Temp | R |
| 2118 | Compressor Frequency | R |
| 2135 | System Status | R |
| 2137 | Error Code | R |
| 2138 | Protection Status | R |

### Working Modes (Register 2001)

| Value | Mode |
|-------|------|
| 0 | Cooling |
| 1 | Underfloor Heating |
| 2 | Fan Coil Heating |
| 5 | Hot Water |
| 6 | Auto |

---

## Error Codes Reference

The heat pump displays error codes on the wired controller. These codes map to specific bits in the error registers (2134, 2137, 2138).

### Sensor Errors (E-Series)

| Code | Description | Register | Bit | Resolution |
|------|-------------|----------|-----|------------|
| E01 | Compressor discharge temperature sensor fault | 2137 | 6 | Check if the compressor discharge temperature sensor for short or open circuit and correct or replace. |
| E05 | Heat pump coil temperature sensor fault | 2137 | 5 | Check the heat pump coil temperature sensor and wires for a short or open circuit and correct or replace sensors. |
| E09 | Compressor suction temperature sensor fault | 2137 | 7 | Check if the compressor suction temperature sensor for short or open circuit and correct or replace. |
| E13 | Cooling coil temperature sensor fault | 2137 | 4 | Check the coil temperature sensor for a short or open circuit and correct or replace. |
| E18 | Outlet water temperature sensor fault | 2137 | 3 | Check the outlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace. |
| E19 | Inlet water temperature sensor fault | 2137 | 2 | Check the inlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace. |
| E20 | Water tank temperature sensor fault | 2134 | 3 | Check if the hot water tank temperature sensor or wires have a short or open circuit. Correct or replace. |
| E21 | Wired controller communication fault | 2137 | 10 | Check the wired controller's cable and its connections. |
| E22 | Outdoor ambient temperature sensor fault | 2137 | 8 | Check if the ambient temperature sensor for the heat pump or its wiring has a short or open circuit and correct or replace. |
| E28 | Outdoor EEPROM fault | 2137 | 1 | Contact the dealer. |
| E33 | High pressure sensor fault | 2138 | 6 | Compressor high pressure switch or wiring faulty. Correct or replace. |
| E34 | EEV return sensor fault | 2137 | 7 | Check if the suction temperature sensor short circuit or disconnect. |

### Protection Errors (P-Series)

| Code | Description | Register | Bit | Resolution |
|------|-------------|----------|-----|------------|
| P01 | Water flow switch protection | 2138 | 8 | Flow is too low or wiring is open circuit. Check the water system, water pump, and operation of water flow switch and correct problem. |
| P02 | High pressure protection activated | 2138 | 6 | 1) Check whether the water temperature is too high or blocked. 2) Check whether the fan blades are blocked or if evaporator fins are blocked impacting heat transfer efficiency. 3) Check whether snow or ice has built up too much inside the unit. 4) Check that the water tank temperature setting is not too high. |
| P06 | Low pressure protection activated | 2138 | 7 | 1) Check whether the unit is leaking refrigerant. 2) Repair and vacuum system, then refill with exact amount of refrigerant as per nameplate. |
| P11 | Compressor discharge temperature too high | 2138 | 5 | 1) Check water system is operating normal, look for reduction in normal water flow. 2) Check whether there was a refrigerant leak and repair. 3) Verify unit is in normal operation with proper exhaust temperature and system pressure. |
| P15 | Inlet/outlet temperature difference too large | 2138 | 13 | 1) Check if water system is operating abnormally, such as water flow is too low. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure. |
| P16 | Outlet water temperature too low protection | 2138 | 14 | 1) Check water system is normal and water flow is adequate. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure. |
| P19 | AC current protection | 2138 | 0 | Contact the dealer. |
| P27 | Cooling coil temperature overheating protection | 2138 | 9 | Check that the fan is in good condition and that the evaporator fins are not in need of cleaning. |
| P30 | Antifreeze cooling coil protection | 2137 | 4 | Unit antifreeze protection activated. Wait for conditions to improve or check for issues. |
| PA | Tank temperature protection activated | 2137 | 14 | Contact the dealer. |

### Compressor/Inverter Errors (r-Series)

| Code | Description | Register | Bit | Resolution |
|------|-------------|----------|-----|------------|
| r01 | IPM module fault | 2137 | 13 | Contact the dealer. |
| r02 | Compressor start fault | 2137 | 11 | Contact the dealer. |
| r05 | IPM module temperature too high | 2138 | 4 | Contact the dealer. |
| r06 | Compressor phase current protection | 2138 | 1 | This applies to 3-phase units where the phasing of the wires is incorrect and needs to be corrected. |
| r10 | AC voltage too high or too low | 2137 | 15 | Contact the dealer. |
| r11 | DC bus voltage protection | 2138 | 3 | Contact the dealer. |
| r20 | Compressor protection | 2138 | 15 | Contact the dealer. |

### Fan/Motor Errors (F-Series)

| Code | Description | Register | Bit | Resolution |
|------|-------------|----------|-----|------------|
| FA | DC fan motor protection | 2138 | 2 | Contact the dealer. |

### Pressure Errors (E/F-Series)

| Code | Description | Register | Bit | Resolution |
|------|-------------|----------|-----|------------|
| EB | High pressure protection (pressure sensor) | 2138 | 6 | Check if the ambient temperature sensor is short circuit or disconnected. |
| EC | EEV circuit low pressure protection | 2138 | 11 | 1) Check whether the unit is leaking refrigerant. 2) After leak repair and vacuum, refill with correct refrigerant amount per nameplate. |
| ED | Low pressure protection (pressure sensor) | 2138 | 7 | Check if the ambient temperature sensor is short circuit or disconnected. |
| FE | Startup differential pressure protection | 2138 | 15 | Contact the dealer. |
| FF | Running differential protection | 2138 | 15 | Contact the dealer. |

### Register Bit to Error Code Mapping

#### Register 2134 - Error Code 1

| Bit | Arctic Code | Description |
|-----|-------------|-------------|
| 0 | - | Brine side inlet water temperature sensor error |
| 1 | - | Brine side outlet water temperature sensor error |
| 2 | - | Brine side water flow protection |
| 3 | E20 | Water tank temperature sensor error |
| 4-15 | - | Reserved |

#### Register 2137 - Error Code 2

| Bit | Arctic Code | Description |
|-----|-------------|-------------|
| 0 | - | Indoor EE error |
| 1 | E28 | Outdoor EE error |
| 2 | E19 | Inlet water temp sensor error |
| 3 | E18 | Outlet water temp sensor error |
| 4 | P30/E13 | Cooling coil antifreeze/temp sensor |
| 5 | E05 | External coil temp sensor error |
| 6 | E01 | Discharge temperature sensor error |
| 7 | E09/E34 | Suction temperature sensor error |
| 8 | E22 | Ambient temperature sensor error |
| 9 | - | Communication error between drive board and main board |
| 10 | E21 | Wired controller communication error |
| 11 | r02 | Compressor abnormal start |
| 12 | - | Communication error between indoor and outdoor unit |
| 13 | r01 | IPM error |
| 14 | PA | High outlet water temperature protection |
| 15 | r10 | AC voltage protection |

#### Register 2138 - Error Code 3

| Bit | Arctic Code | Description |
|-----|-------------|-------------|
| 0 | P19 | AC current protection |
| 1 | r06 | Compressor current protection |
| 2 | FA | DC fan motor protection |
| 3 | r11 | Bus voltage protection |
| 4 | r05 | IPM temperature protection |
| 5 | P11 | High discharge temperature protection |
| 6 | P02/E33/EB | High pressure switch protection |
| 7 | P06/ED | Low pressure switch protection |
| 8 | P01 | Water flow switch protection |
| 9 | P27 | Cooling external coil overheat protection |
| 10 | - | Low ambient temperature protection |
| 11 | EC | Primary circuit low pressure protection |
| 12 | - | Secondary circuit low pressure protection |
| 13 | P15 | Large inlet/outlet temperature difference protection |
| 14 | P16 | Low outlet water temperature protection |
| 15 | r20/FE/FF | Compressor running differential |

---

# Arctic Heat Pump Simulator

Modbus RTU slave emulator for the Arctic ECO-600 heat pump, running on an M5Stack Atom S3 (ESP32-S3) with an RS-485 adapter. Designed for integration testing with the [Arctic Controller](https://github.com/sslivins/arctic-controller).

## Features

- **Modbus RTU Slave** — Responds to FC 0x03 (read), FC 0x06 (write single), FC 0x10 (write multiple) at 2400 baud, 8E1
- **REST API** — Set register values, load presets, control playback via HTTP
- **Presets** — One-click simulation of common states: idle, heating, cooling, hot water, defrost, error conditions
- **Playback** — Load JSONL capture files and replay real heat pump communication patterns
- **mDNS** — Accessible at `arctic-sim.local`

## Hardware

| Component | Model |
|-----------|-------|
| MCU | M5Stack Atom S3 (ESP32-S3) |
| RS-485 | Atomic RS485 Base or equivalent |
| Communication | 2400 baud, 8-Even-1, half-duplex RS-485 |

## Quick Start

### Prerequisites

- ESP-IDF v5.4.x installed and sourced
- M5Stack Atom S3 connected via USB

### Build & Flash

```bash
idf.py set-target esp32s3
idf.py menuconfig    # Set WiFi SSID/password and RS-485 GPIO pins
idf.py build flash monitor
```

### Configuration (menuconfig)

Under **Arctic Simulator Configuration**:

| Setting | Default | Description |
|---------|---------|-------------|
| WiFi SSID | *(empty)* | Your WiFi network name |
| WiFi Password | *(empty)* | Your WiFi password |
| mDNS Hostname | `arctic-sim` | Accessible as `arctic-sim.local` |
| RS-485 TX GPIO | 6 | Adjust for your RS-485 adapter |
| RS-485 RX GPIO | 5 | Adjust for your RS-485 adapter |
| RS-485 DIR GPIO | -1 | Direction control pin (-1 = auto) |
| UART Port | 1 | UART peripheral to use |
| Modbus Slave Address | 1 | Must match controller config |

## REST API

Base URL: `http://arctic-sim.local`

### Status

```
GET /api/status
```

Returns simulator status, Modbus statistics, and playback state.

### Registers

```
GET  /api/registers              # All register values
GET  /api/registers?addr=2100    # Single register
PUT  /api/registers?addr=2110    # Set single register
     Body: { "value": 350 }
POST /api/registers/bulk         # Set multiple registers
     Body: { "registers": { "2100": 350, "2110": 200 } }
```

### Presets

```
POST /api/preset
Body: { "name": "heating" }
```

Available presets: `idle`, `heating`, `cooling`, `hot_water`, `defrost`, `error_e01`, `error_p01`

### Error Control

```
POST /api/errors/clear           # Clear all error flags
POST /api/reboot                 # Reboot the device
```

### Playback

```
POST /api/playback/load          # Upload JSONL capture data (body)
POST /api/playback/start         # Start playback
POST /api/playback/stop          # Stop playback
GET  /api/playback/status        # Playback state and position
```

## Capture File Format (JSONL)

One JSON object per line. Each entry represents a register snapshot at a point in time.

```jsonl
{"t":0,"fc":3,"addr":2100,"count":39,"values":[200,0,250,250,250,30,0,20,0,0,50,0,0,0,250,0,0,0,55,200,12,8,1,0,200,0,250,80,1,0,0,0,0,0,0,33,0,0,0]}
{"t":500,"fc":3,"addr":2000,"count":58,"values":[1,1,70,450,550,50,50,50,50,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,250,200,0,0,0,0,37,38,0,0,0,43,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}
{"t":1000,"fc":6,"addr":2000,"value":1}
```

Fields:
- `t` — Milliseconds since capture start (controls replay timing)
- `fc` — Modbus function code (3=read, 6=write single, 16=write multiple)
- `addr` — Starting register address
- `count` / `values` — For multi-register entries
- `value` — For single-register writes

## Register Map

The register semantics below were reverse-engineered from live captures of the
real Macon controller. `main/register_map.h` is the authoritative, in-repo
source of truth (named constants + per-bit notes).

The unit exposes two register windows on the Tuya/Macon wire:

- **Holding** window (wire `addr=50`): regs **2000–2057**.
- **Telemetry** window (wire `addr=0`): regs **2093–2142**. Byte 0 of this
  window is **reg2093 = the cooling setpoint** (formerly mistaken for an opaque
  7-byte prefix; corrected in arctic-macon 311a291).

### Key Registers

| Address | Window | Description | R/W |
|---------|--------|-------------|-----|
| 2000 | Holding | A4 · AC input current (A) | R |
| 2001 | Holding | A7 · DC bus voltage (×10 = V) | R |
| 2003 | Holding | A10 · DC fan motor speed | R |
| 2007 | Holding | Operating-state / fault bitfield (bit5 `0x20` = hot-water running) | R/W |
| 2008 | Holding | o1 · Water tank temp (°C) | R |
| 2012 | Holding | Hot water setpoint (°C) | R/W |
| 2093 | Telemetry | Cooling setpoint (whole °C) | R/W |
| 2101 | Telemetry | A13 · AC input voltage (×10 = V) | R |
| 2104 | Telemetry | A5 · Main EEV position (steps) | R |
| 2113 | Telemetry | A8 · IPM module temp (°C) | R |
| 2114 | Telemetry | A9 · Real-time power (×100 = W) | R |
| 2125–2128 | Telemetry | Fault bitfields (sensor/EE, comm/compressor, electrical, refrigerant/P-codes) | R |
| 2129 | Telemetry | Icon bitfield #2 (defrost, fan) | R |
| 2130 | Telemetry | Icon bitfield #1 (compressor, pump, heating) | R |
| 2132 | Telemetry | o3 · Outlet (supply) water temp (°C) | R |
| 2133 | Telemetry | o2 · Inlet (return) water temp (°C) | R |
| 2134 | Telemetry | o4 · Outdoor ambient temp (°C) | R |
| 2135 | Telemetry | A6 · Cool coil temp (°C) | R |
| 2136 | Telemetry | A3 · Suction temp (°C) | R |
| 2137 | Telemetry | A2 · Coil temp (°C) | R |
| 2138 | Telemetry | A1 · Discharge temp (°C) | R |
| 2141 | Telemetry | A14 · Compressor frequency (Hz) | R |

> The R/W column reflects the real unit's protocol. In the **simulator** every
> register is settable via `PUT /api/registers` / `POST /api/registers/bulk`.
> The controller writes the setpoint with an fc=0x06 command (`addr=0`), which
> the simulator reflects back into telemetry reg2093.

## Project Structure

```
arctic-simulator/
├── CMakeLists.txt              # Top-level CMake
├── sdkconfig.defaults          # Default build config
├── partitions.csv              # Flash partition table
├── main/
│   ├── CMakeLists.txt          # Component registration
│   ├── Kconfig.projbuild       # Menuconfig options
│   ├── idf_component.yml       # mdns, led_strip dependencies
│   ├── main.cpp                # Entry point, task creation
│   ├── register_map.h/cpp      # Register storage, presets
│   ├── tuya_codec/             # Tuya MCU framing (vendored from sniffer)
│   ├── tuya_state.h/cpp        # Byte-store + mutex-guarded snapshots
│   ├── tuya_slave.h/cpp        # Tuya MCU slave over RS-485
│   ├── api_server.h/cpp        # REST API (esp_http_server)
│   ├── playback.h/cpp          # JSONL capture replay engine
│   └── wifi_manager.h/cpp      # WiFi STA + mDNS
└── captures/
    └── example.jsonl           # Example capture file
```

## License

Private — same terms as arctic-controller.

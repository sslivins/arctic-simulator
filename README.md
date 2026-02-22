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
{"t":0,"fc":3,"addr":2100,"count":39,"values":[200,0,250,250,750,30,0,20,0,0,50,0,0,0,450,0,0,0,55,600,230,8,3200,0,200,0,250,80,1,0,0,0,0,0,0,33,0,0,0]}
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

See the [Arctic Modbus Protocol](https://github.com/sslivins/arctic-controller/blob/main/docs/ARCTIC-MODBUS-PROTOCOL.md) for the complete register map.

### Key Registers

| Address | Description | R/W |
|---------|-------------|-----|
| 2000 | Unit ON/OFF | R/W |
| 2001 | Working Mode (0=Cool, 1=FloorHeat, 2=FanCoil, 5=HotWater, 6=Auto) | R/W |
| 2002–2004 | Temperature Setpoints (cool/heat/hot water) | R/W |
| 2100 | Water Tank Temperature | R |
| 2102–2103 | Outlet / Inlet Water Temperature | R |
| 2110 | Outdoor Ambient Temperature | R |
| 2118 | Compressor Frequency (Hz) | R |
| 2119 | Fan Speed (RPM) | R |
| 2135 | System Status (bit field) | R |
| 2137 | Error Code 2 (bit field) | R |
| 2138 | Error Code 3 / Protection (bit field) | R |

## Project Structure

```
arctic-simulator/
├── CMakeLists.txt              # Top-level CMake
├── sdkconfig.defaults          # Default build config
├── partitions.csv              # Flash partition table
├── main/
│   ├── CMakeLists.txt          # Component registration
│   ├── Kconfig.projbuild       # Menuconfig options
│   ├── idf_component.yml       # esp-modbus, mdns dependencies
│   ├── main.cpp                # Entry point, task creation
│   ├── register_map.h/cpp      # Register storage, presets
│   ├── modbus_slave.h/cpp      # Modbus RTU slave (esp_modbus)
│   ├── api_server.h/cpp        # REST API (esp_http_server)
│   ├── playback.h/cpp          # JSONL capture replay engine
│   └── wifi_manager.h/cpp      # WiFi STA + mDNS
└── captures/
    └── example.jsonl           # Example capture file
```

## License

Private — same terms as arctic-controller.

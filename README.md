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

### Semantic state (preferred)

Every field, flag, enum and fault is named by the **arctic-macon** library
(`components/arctic-macon`, `macon_fields.h` / `macon_faults.h`) -- the same
library the controller decodes with. The simulator holds no register map of
its own, so the two can't drift apart. `GET /api/status` reports the library's
`macon.api_version` plus `layout_fingerprint` / `catalog_fingerprint`; a
consumer should refuse to run if they differ from its own.

```
GET   /api/fields                # Catalog: every named field (kind, unit, range, step, enum keys)
GET   /api/state                 # Current value of every named field, decoded operation, active faults
PATCH /api/state                 # Atomic multi-field update (all-or-nothing, 400 with reason on reject)
      Body: { "outlet_water_temp": 42, "pump_on": true, "working_mode": "floor_heating" }
GET   /api/heatpump              # Compact summary (decoded by the library)
```

Numbers are semantic units (deg C, Hz, RPM, W, V); flags accept `true`/`false`
or `0`/`1`; enums take the string keys listed by `/api/fields`.

### Faults

```
GET  /api/faults/catalog         # Every OEM code: id, code, label, severity, resolution, sites[]
GET  /api/faults                 # Active faults (code, label, severity, site)
POST /api/faults                 # { "code": "P01", "active": true }  -- every site of the code
                                 # { "site": 12,    "active": true }  -- one specific bit
POST /api/faults/clear           # Clear every fault (keeps the unit-enabled flag)
```

### Presets

```
POST /api/preset
Body: { "name": "heating" }
```

Available presets: `idle`, `heating`, `cooling`, `hot_water`, `defrost`, `fault_p01`.
Each preset is a list of named fields applied atomically on top of a common
idle baseline, so nothing leaks from the previous preset.

### Bench lease (advisory)

```
GET    /api/lease                # Current holder, if any
POST   /api/lease                # { "owner": "ci-run-123", "ttl_s": 900 }  -- 409 if held by someone else
DELETE /api/lease                # { "owner": "ci-run-123" } or { "force": true }
```

### Controller commands

```
GET  /api/commands               # Recent fc=0x06 writes received from the controller
                                 # (wire_addr, data bytes, applied, raw frame)
```

### Raw registers (debug only)

```
GET  /api/registers              # All served register values, grouped by window
GET  /api/registers?addr=2100    # Single register
PUT  /api/registers?addr=2110    # Set single register (low byte stored)
     Body: { "value": 35 }
POST /api/registers/bulk         # Set multiple registers
     Body: { "registers": { "2100": 35, "2110": 20 } }
POST /api/errors/clear           # Legacy alias of /api/faults/clear
POST /api/reboot                 # Reboot the device
```

Prefer the semantic endpoints; raw register access bypasses the library's
meaning and exists only for protocol debugging.

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

The simulator no longer carries a register map. Every address, bit and scaling
is defined once in the **arctic-macon** library -- see
`components/arctic-macon/docs/REGISTERS.md` and `include/macon_fields.h` --
and shared with the controller and sniffer.

The unit exposes two register windows on the Tuya/Macon wire:

- **Holding** window (wire `addr=50`): regs **2000-2057**.
- **Telemetry** window (wire `addr=0`): regs **2093-2142**.

At boot the store is seeded with payloads captured from a real Macon mainboard
(2026-05-03), so bytes the library doesn't name still match the real unit, and
then the `idle` preset is applied. Controller fc=0x06 writes are applied with
the library's `MaconImage::apply_write`, exactly as the real unit reflects
them.

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

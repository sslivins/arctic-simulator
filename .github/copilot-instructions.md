# Copilot Instructions — Arctic Simulator

## Conversation Preferences

- **Do not use popup selection dialogs** (the `ask_questions` tool). Ask questions
  directly in the chat and wait for a response. The user prefers inline conversation.
- Be direct and concise. Skip preamble like "I'll now…" — just do the work.
- When multiple approaches exist, pick the best one and proceed. Only ask if the
  choice has significant irreversible consequences.
- After completing file operations, confirm briefly rather than restating what was done.
- When committing, write clear **conventional-commit** messages:
  - `feat:` — new feature or capability
  - `fix:` — bug fix
  - `docs:` — documentation only
  - `ci:` — CI/workflow changes
  - `refactor:` — code restructuring (no behavior change)
  - `test:` — adding or updating tests
  - `chore:` — maintenance tasks
  - Scoped variants are fine: `fix(modbus):`, `feat(api):`
- **Always work on a feature branch** — never commit directly to `main`. Create a
  branch (e.g. `feat/sniffer`, `fix/uart-pins`) before making changes. The user
  will merge via PR.
- When asked for a PR description, always output it in **Markdown** format.

## Project Overview

Arctic Simulator is a **Modbus RTU slave emulator** for the Arctic ECO-600 heat pump.
It runs on an **M5Stack Atom S3** (ESP32-S3) with an RS-485 adapter, acting as a
stand-in for the real heat pump during development and integration testing of the
[Arctic Controller](https://github.com/sslivins/arctic-controller).

- **Framework**: ESP-IDF v5.4.3
- **Target**: ESP32-S3 (M5Stack Atom S3 / Atom S3R)
- **Language**: C++
- **Communication**: Modbus RTU slave at 2400 baud, 8-Even-1, RS-485 half-duplex
- **Slave Address**: 1 (matches real heat pump)
- **Protocol**: EVI DC Inverter Heat Pump Communication Protocol V1.3

### Operational Modes

1. **Interactive** — REST API controls register values; Modbus slave serves them
   to the controller on demand. Use presets to quickly switch between heating,
   cooling, error states, etc.

2. **Playback** — Load a JSONL capture file (recorded from a real heat pump via
   the future sniffer tool). The simulator replays register snapshots with original
   timing so the controller sees realistic communication patterns.

## Repository Structure

| Path | Purpose |
|------|---------|
| `main/` | All application source code |
| `main/register_map.h/cpp` | Register storage (holding 2000–2057, input 2100–2138), presets |
| `main/modbus_slave.h/cpp` | Modbus RTU slave via esp_modbus, RS-485 communication |
| `main/api_server.h/cpp` | REST API endpoints (register control, presets, playback) |
| `main/playback.h/cpp` | JSONL capture file parser and replay engine |
| `main/wifi_manager.h/cpp` | WiFi STA mode + mDNS (arctic-sim.local) |
| `main/main.cpp` | Entry point, FreeRTOS task creation |
| `main/Kconfig.projbuild` | Menuconfig options (WiFi, GPIO pins, Modbus address) |
| `captures/` | Example JSONL capture files |
| `.github/workflows/` | CI: `build.yml` |

## Related Projects

- **[arctic-controller](https://github.com/sslivins/arctic-controller)** — The heat
  pump controller this simulator is designed to test against. The controller is the
  Modbus master; this simulator is the slave.
- The Modbus register map is defined in the controller repo at
  `docs/ARCTIC-MODBUS-PROTOCOL.md`. Any register map changes must be kept in sync.

## Code Conventions

### C++ / Firmware
- Follow ESP-IDF patterns: `ESP_LOGI/LOGW/LOGE` for logging, `esp_err_t` returns
- **Printf format specifiers**: The Xtensa/RISC-V toolchain does not reliably handle
  `%lld` for `int64_t`. Always cast to `(long)` and use `%ld`, or `(unsigned long)`
  and use `%lu`.
- Namespaces: `reg::`, `mb_slave::`, `api::`, `playback::`, `wifi::`
- Register addresses use the protocol's native numbering (2000–2138), not zero-based
  offsets. Conversion to array indices is internal to `register_map.cpp`.

### REST API
- All endpoints under `/api/`
- CORS enabled (Access-Control-Allow-Origin: *)
- JSON request/response bodies
- Endpoints:
  - `GET /api/status` — simulator info + Modbus stats + playback state
  - `GET /api/registers` — all registers
  - `GET /api/registers?addr=XXXX` — single register
  - `PUT /api/registers?addr=XXXX` — set register `{ "value": N }`
  - `POST /api/registers/bulk` — set multiple `{ "registers": { "2100": 350 } }`
  - `POST /api/preset` — load preset `{ "name": "heating" }`
  - `POST /api/errors/clear` — clear error flags
  - `POST /api/playback/load` — upload JSONL capture (body)
  - `POST /api/playback/start` — start playback
  - `POST /api/playback/stop` — stop playback
  - `GET /api/playback/status` — playback state

### Capture File Format (JSONL)
One JSON object per line:
```jsonl
{"t":0,"fc":3,"addr":2100,"count":39,"values":[...]}
{"t":500,"fc":6,"addr":2000,"value":1}
```
- `t` — milliseconds since capture start
- `fc` — function code (3=read holding, 6=write single)
- `addr` — starting register address
- `count`/`values` — multi-register data
- `value` — single-register write

### Hardware Configuration
Default GPIO pins for Atom S3 + RS485 base (adjust via menuconfig):
- TX: GPIO 6
- RX: GPIO 5
- DIR: -1 (auto direction control)
- UART port: 1

## CI / Build

- **CI workflow**: `.github/workflows/build.yml` builds in `espressif/idf:v5.4.3`
  container with target `esp32s3`
- **Path filters**: Only `main/`, `CMakeLists.txt`, `sdkconfig.defaults`,
  `partitions.csv`, and workflow files trigger builds

### Web Dashboard (gzip rebuild)

The dashboard HTML (`main/web/index.html`) is gzip-compressed and embedded in the
firmware. The compression runs during **CMake configure** (not during ninja build),
so editing the HTML and running `idf.py build` alone will **not** pick up changes.

After editing `main/web/index.html`, do one of:

```bash
# Option A: manually re-gzip then build
python main/web/gzip_html.py main/web/index.html main/web/index.html.gz
idf.py build

# Option B: force CMake reconfigure (slower, re-runs full configure)
idf.py reconfigure
idf.py build
```

## After Major Changes — Checklist

- [ ] Keep register map in sync with `arctic-controller/docs/ARCTIC-MODBUS-PROTOCOL.md`
- [ ] Update `README.md` if API endpoints or presets changed
- [ ] Update example capture files if JSONL format changed
- [ ] Re-gzip `index.html` if the dashboard was modified (see above)
- [ ] Verify build passes with `idf.py build`
- [ ] Use conventional commit messages

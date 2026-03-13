# Arctic Sniffer

Passive Modbus RTU bus monitor for the **Arctic ECO-600** heat pump communication
protocol.  Runs on an **M5Stack Atom S3** (ESP32-S3) with an RS-485 adapter,
connected to the same bus as the heat pump (or
[arctic-simulator](https://github.com/sslivins/arctic-simulator)) and the
[arctic-controller](https://github.com/sslivins/arctic-controller).

The sniffer **never transmits** — it only listens.

## Features

| Mode | Description |
|------|-------------|
| **Monitor** | Live web dashboard at `http://arctic-sniffer.local` showing decoded Modbus transactions streamed via WebSocket. Each transaction shows the function code, register addresses, decoded names, and human-readable values. |
| **Record** | Captures Modbus traffic to JSONL format compatible with [arctic-simulator](https://github.com/sslivins/arctic-simulator) playback mode. Recordings are buffered in memory and downloadable via the REST API. |

## Hardware

- **MCU**: M5Stack Atom S3 (ESP32-S3) or Atom S3R
- **RS-485**: M5Stack RS485 Base or equivalent transceiver
- **Bus**: Connected in parallel to the existing master ↔ slave RS-485 bus
- **Baud**: 2400 baud, 8-Even-1 (matches the Arctic protocol)

Default GPIO pins (adjustable via `idf.py menuconfig`):

| Signal | GPIO | Notes |
|--------|------|-------|
| RX     | 5    | RS-485 receive |
| TX     | 6    | Wired but unused (receive-only) |
| UART   | 1    | UART peripheral number |

## Quick Start

```bash
# Clone
git clone https://github.com/sslivins/arctic-sniffer.git
cd arctic-sniffer

# Configure WiFi credentials
idf.py menuconfig
# → Arctic Sniffer Configuration → WiFi SSID / Password

# Build & flash
idf.py build flash monitor
```

Once connected to WiFi the device announces itself via mDNS as
**arctic-sniffer.local**.

## REST API

All endpoints return/accept JSON.  CORS is enabled.

[**Interactive API docs (Swagger UI)**](https://petstore.swagger.io/?url=https://raw.githubusercontent.com/sslivins/arctic-sniffer/main/docs/openapi.yaml) ·
[OpenAPI spec](docs/openapi.yaml)

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET`  | `/api/status` | Sniffer stats, recording state |
| `GET`  | `/api/log` | Recent decoded transactions (JSON array) |
| `POST` | `/api/record/start` | Begin recording |
| `POST` | `/api/record/stop` | Stop recording |
| `GET`  | `/api/record/download` | Download JSONL capture |
| `DELETE` | `/api/record` | Clear recorded data |
| `GET`  | `/ws` | WebSocket — live transaction stream |

## Capture File Format (JSONL)

One JSON object per line, compatible with arctic-simulator playback:

```jsonl
{"t":0,"fc":3,"addr":2100,"count":39,"values":[0,1,2,...]}
{"t":500,"fc":6,"addr":2000,"value":1}
```

| Field | Description |
|-------|-------------|
| `t` | Milliseconds since recording start |
| `fc` | Function code (3 = read holding, 6 = write single, 16 = write multiple) |
| `addr` | Starting register address |
| `count` / `values` | Multi-register read/write data |
| `value` | Single-register write |

## Web Dashboard

The dashboard HTML is gzip-compressed and embedded in the firmware.  After
editing `main/web/index.html`:

```bash
# Option A: manually re-gzip then build
python main/web/gzip_html.py main/web/index.html main/web/index.html.gz
idf.py build

# Option B: force CMake reconfigure (re-runs full configure)
idf.py reconfigure
idf.py build
```

## Project Structure

| Path | Purpose |
|------|---------|
| `main/main.cpp` | Entry point, FreeRTOS task creation |
| `main/modbus_sniffer.h/cpp` | Raw UART reception, frame detection, Modbus RTU parsing |
| `main/arctic_registers.h/cpp` | Register definitions, names, units, value decoding |
| `main/recorder.h/cpp` | JSONL recording engine (memory-buffered) |
| `main/api_server.h/cpp` | REST API + WebSocket server |
| `main/wifi_manager.h/cpp` | WiFi STA mode + mDNS |
| `main/web/index.html` | Embedded web dashboard |
| `main/Kconfig.projbuild` | Menuconfig options (WiFi, GPIO pins) |

## Related Projects

- **[arctic-controller](https://github.com/sslivins/arctic-controller)** — Modbus
  master (heat pump controller) on ESP32-P4 / M5Stack Tab5.
- **[arctic-simulator](https://github.com/sslivins/arctic-simulator)** — Modbus
  slave emulator. The sniffer's JSONL recording format is designed for direct
  playback in the simulator.

## License

MIT

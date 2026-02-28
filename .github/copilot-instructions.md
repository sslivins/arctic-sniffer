# Copilot Instructions — Arctic Sniffer

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
  branch (e.g. `feat/web-ui`, `fix/crc-check`) before making changes. The user
  will merge via PR.
- When asked for a PR description, always output it in **Markdown** format.

## Project Overview

Arctic Sniffer is a **passive Modbus RTU bus monitor** for the Arctic ECO-600 heat
pump communication protocol. It runs on an **M5Stack Atom S3** (ESP32-S3) with an
RS-485 adapter, connected to the same RS-485 bus as the heat pump (or simulator)
and controller. It never transmits — only listens.

- **Framework**: ESP-IDF v5.4.3
- **Target**: ESP32-S3 (M5Stack Atom S3 / Atom S3R)
- **Language**: C++
- **Communication**: Passive RS-485 listener at 2400 baud, 8-Even-1
- **Protocol**: EVI DC Inverter Heat Pump Communication Protocol V1.3

### Operational Modes

1. **Monitor** (default) — Live web dashboard showing decoded Modbus transactions
   streamed via WebSocket. Each transaction displays the function code, register
   addresses, decoded register names, and human-readable values.

2. **Record** — Captures Modbus traffic to JSONL format compatible with the
   [Arctic Simulator](https://github.com/sslivins/arctic-simulator) playback mode.
   Recordings are buffered in memory and downloadable via the REST API.

### How It Works

The sniffer passively receives all bytes on the RS-485 bus. It detects Modbus RTU
frame boundaries using inter-frame silence (≥ 3.5 character times = ~16 ms at
2400 baud). A state machine pairs master requests with slave responses to produce
complete decoded transactions. These are:
- Pushed to connected WebSocket clients as JSON for the live dashboard
- Optionally accumulated as JSONL entries when recording is active

## Repository Structure

| Path | Purpose |
|------|---------|
| `main/` | All application source code |
| `main/arctic_registers.h/cpp` | Register definitions, names, units, value decoding |
| `main/modbus_sniffer.h/cpp` | Raw UART reception, frame detection, Modbus RTU parsing |
| `main/recorder.h/cpp` | JSONL recording engine (memory-buffered) |
| `main/api_server.h/cpp` | REST API + WebSocket server |
| `main/wifi_manager.h/cpp` | WiFi STA mode + mDNS (arctic-sniffer.local) |
| `main/main.cpp` | Entry point, FreeRTOS task creation |
| `main/Kconfig.projbuild` | Menuconfig options (WiFi, GPIO pins) |
| `main/web/` | Embedded web dashboard (HTML, gzip script) |
| `.github/workflows/` | CI: `build.yml` |

## Related Projects

- **[arctic-controller](https://github.com/sslivins/arctic-controller)** — The Modbus
  master (heat pump controller) running on ESP32-P4 / M5Stack Tab5.
- **[arctic-simulator](https://github.com/sslivins/arctic-simulator)** — The Modbus
  slave emulator. The sniffer's recording format (JSONL) is designed to be directly
  loaded into the simulator's playback mode.
- The Modbus register map is defined in the controller repo at
  `docs/ARCTIC-MODBUS-PROTOCOL.md`. Any register map changes must be kept in sync.

## Code Conventions

### C++ / Firmware
- Follow ESP-IDF patterns: `ESP_LOGI/LOGW/LOGE` for logging, `esp_err_t` returns
- **Printf format specifiers**: The Xtensa/RISC-V toolchain does not reliably handle
  `%lld` for `int64_t`. Always cast to `(long)` and use `%ld`, or `(unsigned long)`
  and use `%lu`.
- Namespaces: `arctic::`, `sniffer::`, `recorder::`, `api::`, `wifi::`
- Register addresses use the protocol's native numbering (2000–2138), not zero-based
  offsets.
- Modbus CRC16 is computed locally (no dependency on esp-modbus for passive sniffing).

### REST API
- All endpoints under `/api/`
- CORS enabled (Access-Control-Allow-Origin: *)
- JSON request/response bodies
- Endpoints:
  - `GET /api/status` — sniffer info, stats, recording state
  - `GET /api/log` — recent decoded transactions (JSON array)
  - `POST /api/record/start` — begin recording
  - `POST /api/record/stop` — stop recording
  - `GET /api/record/download` — download JSONL capture data
  - `DELETE /api/record` — clear recorded data
- WebSocket: `GET /ws` — live transaction stream

### Capture File Format (JSONL)
One JSON object per line, compatible with arctic-simulator playback:
```jsonl
{"t":0,"fc":3,"addr":2100,"count":39,"values":[...]}
{"t":500,"fc":6,"addr":2000,"value":1}
```
- `t` — milliseconds since recording start
- `fc` — function code (3=read holding, 6=write single, 16=write multiple)
- `addr` — starting register address
- `count`/`values` — multi-register data
- `value` — single-register write

### Hardware Configuration
Default GPIO pins for Atom S3 + RS485 base (adjust via menuconfig):
- TX: GPIO 6 (active but unused — sniffer is receive-only)
- RX: GPIO 5
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
- [ ] Update `README.md` if API endpoints or features changed
- [ ] Re-gzip `index.html` if the dashboard was modified (see above)
- [ ] Verify build passes with `idf.py build`
- [ ] Use conventional commit messages

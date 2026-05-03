# Arctic Sniffer

Passive serial bus monitor for the **Arctic ECO-600** heat pump communication
protocol.  Runs on an **M5Stack Atom S3** (ESP32-S3) with an RS-485 adapter,
connected to the same bus as the heat pump (or
[arctic-simulator](https://github.com/sslivins/arctic-simulator)) and the
[arctic-controller](https://github.com/sslivins/arctic-controller).

The sniffer **never transmits** — it only listens.

> **Wire-protocol update (May 2026):** Live captures show the Arctic ECO-600
> uses a **Tuya `55 AA`-framed envelope** carrying Arctic register data, **not**
> raw Modbus RTU as the V1.3 protocol PDF describes. See
> [Wire Protocol](#wire-protocol) below for the discovered framing and bus
> settings.

## Features

| Mode | Description |
|------|-------------|
| **Monitor** | Live web dashboard at `http://arctic-sniffer.local` showing decoded Modbus transactions streamed via WebSocket. Each transaction shows the function code, register addresses, decoded names, and human-readable values. |
| **Record** | Captures Modbus traffic to JSONL format compatible with [arctic-simulator](https://github.com/sslivins/arctic-simulator) playback mode. Recordings are buffered in memory and downloadable via the REST API. |

## Hardware

- **MCU**: M5Stack Atom S3 (ESP32-S3) or Atom S3R
- **RS-485**: M5Stack RS485 Base or equivalent transceiver
- **Bus**: Connected in parallel to the existing master ↔ slave RS-485 bus
- **Baud**: 4800 baud, 8-Odd-1 (empirically observed — see [Wire Protocol](#wire-protocol))

Default GPIO pins (adjustable via `idf.py menuconfig`):

| Signal | GPIO | Notes |
|--------|------|-------|
| RX     | 5    | RS-485 receive |
| TX     | 6    | Claimed by the UART so it sits idle-high. **Don't free this pin** — on the M5 Atomic RS485 base the auto-direction transceiver's DE follows TX, so leaving it floating causes DE to bounce and jam the bus (controller logs E21). The sniffer never calls `uart_write`, so nothing actually transmits. |
| UART   | 1    | UART peripheral number |

All four serial parameters (baud, parity, stop bits, RX polarity) are runtime-tunable via `POST /api/config` — useful when probing related but slightly different Arctic / Tuya variants.

## Wire Protocol

This section documents what the Arctic ECO-600 actually puts on the RS-485
bus, as discovered via raw-byte captures (May 2026). It differs from the
Arctic V1.3 protocol PDF in several material ways.

### Bus settings

| Parameter | Documented (V1.3 PDF) | Observed | Notes |
|-----------|-----------------------|----------|-------|
| Baud      | 2400                  | **4800** | 2× the documented rate. |
| Data bits | 8                     | 8        | |
| Parity    | Even                  | **Odd**  | |
| Stop bits | 1                     | 1        | |
| Idle      | high                  | high     | RS-485 differential idle. |

### Frame format

The Arctic V1.3 doc describes plain Modbus RTU. What's actually on the wire is
a **Tuya `55 AA`-style envelope** wrapping Arctic-defined fields:

```
+------+------+-------+-----+-----------+-----------+----------+--------+
| 0x55 | 0xAA |  dir  | fc  |  field A  |  field B  |  data    |  chk   |
| (1B) | (1B) | (1B)  |(1B) |  (2B BE)  |  (2B BE)  |  (B B)   | (1-2B) |
+------+------+-------+-----+-----------+-----------+----------+--------+
```

| Field | Meaning |
|-------|---------|
| `0x55 0xAA` | Tuya frame header. Every frame starts with this. |
| `dir`  | Direction byte. `0xF0` = controller → heat pump (request); `0x0F` = heat pump → controller (response). |
| `fc`   | Function code, mirrors Modbus FC numbering. `0x03` = read register block. (Other codes — `0x06` single write, `0x10` multi-write — not yet observed; expected to follow the same envelope.) |
| `field A` | Starting offset (in bytes) into the unified register page. |
| `field B` | Length (in bytes) of the register region being read/returned. |
| `data` | Present only on responses. Exactly `field B` bytes. **One byte per Arctic register** — not the 2 bytes per register that classic Modbus uses. Apply each register's documented scale factor (see `arctic_registers.cpp`). |
| `chk`  | Trailing checksum. **Algorithm not yet identified**: simple sum-8, ~sum-8, XOR, CRC-8 (poly 0x07, 0x31, etc.) all fail to match. Requests carry **1 byte**, responses carry **2 bytes**. Treated as opaque for now. |

### Register layout

Live polling uses two `fc=0x03` reads in rotation, which together cover a
108-byte unified page:

| `field A` | `field B` | Block | Arctic register range |
|-----------|-----------|-------|-----------------------|
|     0     |    50     | Telemetry / status (input regs) | 2100–2138 (+reserved) |
|    50     |    58     | Setpoints / config (holding regs) | 2000–2057 |

Inside the **telemetry block**, the first **7 bytes are a static prefix**
(`0a 28 32 05 01 00 0f` consistently) — likely device-info / framing
metadata, contents not yet fully decoded. The first Arctic register
(`REG_WATER_TANK_TEMP = 2100`) sits at **byte index 7**. Subsequent bytes
correspond to consecutive Arctic input registers in the order they're
defined in `arctic_registers.h`.

### Example exchange

```
Controller → pump (request):
55 AA F0 03 00 00 00 32 DA
       │  │  └──┬──┘ └──┬──┘ │
       │  │     │       │     └─ checksum (1 B)
       │  │     │       └─ field B = 0x32 = 50 (read 50 bytes)
       │  │     └─ field A = 0   (offset 0 = telemetry block)
       │  └─ fc = 0x03 (read)
       └─ dir = 0xF0 (request)

Pump → controller (response):
55 AA 0F 03 00 00 00 32 [50 bytes register data] [2-byte checksum]
       │
       └─ dir = 0x0F (response)
```

Decoded with `÷2` scale (verified against a known 15 °C water tank reading):

```
byte  reg   name                value
  7   2100  Water Tank Temp     30 ÷ 2 = 15.0 °C   ← matches reality
  9   2102  Outlet Water Temp   raw  6 ÷ 2 =  3.0 °C
 10   2103  Inlet Water Temp    raw  9 ÷ 2 =  4.5 °C
 16   2109  Indoor Ambient      raw 30 ÷ 2 = 15.0 °C
 17   2110  Outdoor Ambient     raw 40 ÷ 2 = 20.0 °C
```

### Open questions

- **Checksum algorithm** — neither sum-8 nor any common CRC-8/CRC-16 variant
  validates. Requests use 1 byte, responses use 2.  Probably a Tuya-specific
  routine; see Tuya MCU documentation for candidates.
- **Static 7-byte prefix in the telemetry block** — looks like device-info or
  a Tuya-style DataPoint header (`0a 28 32 05 01 00 0f`), but the exact
  encoding is undecoded.
- **Write commands (`fc=0x06`/`0x10`)** — not seen in idle-state captures.
  Need a capture during a setpoint change to confirm they share the same
  envelope.
- **Trailing registers 2139–2149** carry real data (e.g. byte 47 `e6` /
  `0xe6`) but are not in the documented Arctic register table.

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

One JSON object per line. Two record types co-exist:

**Decoded transaction** (when the Modbus parser successfully extracts a
classic RTU frame — kept for arctic-simulator compatibility):

```jsonl
{"t":0,"fc":3,"addr":2100,"count":39,"values":[0,1,2,...]}
{"t":500,"fc":6,"addr":2000,"value":1}
```

| Field | Description |
|-------|-------------|
| `t` | Milliseconds since recording start (or boot uptime in raw mode) |
| `fc` | Function code (3 = read holding, 6 = write single, 16 = write multiple) |
| `addr` | Starting register address |
| `count` / `values` | Multi-register read/write data |
| `value` | Single-register write |

**Raw burst** (always emitted, one per UART idle-gap-delimited burst — use
this to analyze Tuya-framed traffic):

```jsonl
{"t":131254,"raw":"55aaf0030032003aa0..."}
```

| Field | Description |
|-------|-------------|
| `t` | Boot-uptime in milliseconds at the end of the burst |
| `raw` | Hex-encoded bytes received in this burst (no decoding applied) |

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

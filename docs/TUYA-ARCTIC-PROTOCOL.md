# Tuya MCU Wire Protocol — Arctic Heat Pump

The Arctic heat pump's RS-485 panel link does **not** speak vanilla
Modbus RTU, despite what the V1.3 PDF implies. It speaks the **Tuya MCU
framing** used by the embedded Tuya Wi-Fi gateway module. This document
is the source of truth for the codec implemented in
[`main/tuya_codec.h`](../main/tuya_codec.h) /
[`main/tuya_codec.cpp`](../main/tuya_codec.cpp).

Validated against real on-site captures (2026-05-03,
`arctic_capture_*.jsonl` and `arctic-simulator/captures/sniff_cap*.jsonl`).

---

## 1. Physical layer

| Param   | Value     |
|---------|-----------|
| Baud    | 2400      |
| Parity  | Even (8E1) |
| Stop    | 1         |
| Wiring  | RS-485, A/B only (no GND in the panel pigtail) |

The gateway polls continuously at ~1 Hz; idle gaps are short.

## 2. Frame layout

```
55 AA <dir:1> <fc:1> <addr:2BE> <count:2BE> [payload:count bytes] <chk:1>
```

| Field   | Width | Notes                                                  |
|---------|-------|--------------------------------------------------------|
| magic   | 2     | Always `55 AA`. Used for resync.                       |
| dir     | 1     | `0xF0` = request (gateway → MCU), `0x0F` = response.   |
| fc      | 1     | Only `0x03` (read register block) observed.            |
| addr    | 2 BE  | Starting **byte offset** into the register page.       |
| count   | 2 BE  | Payload **byte** count (0 on request, N on response).  |
| payload | count | Present only on responses.                             |
| chk     | 1     | Tuya checksum (see §5).                                |

Total length:

| Direction | Bytes              |
|-----------|--------------------|
| Request   | 9 (header + chk, no payload) |
| Response  | 9 + `count`        |

The largest legal frame is well under 256 bytes; the codec caps at
`MAX_FRAME_LEN = 256`.

## 3. Register windows

`addr` and `count` are NOT free-form — only two tuples have been
observed in eight months of captures. The codec rejects everything else
with `UNKNOWN_WINDOW` rather than silently misinterpreting an unexpected
poll.

| `addr` | `count` | Arctic regs covered | Prefix bytes | Notes |
|--------|---------|---------------------|--------------|-------|
| 0      | 50      | 2100 – 2149         | 7            | Telemetry / input regs. First 7 payload bytes are a static header `0a 28 32 05 01 00 0f` — skip them. |
| 50     | 58      | 2000 – 2057         | 0            | Holding regs (config + setpoints). No prefix. |

If new windows ever show up (e.g., writes, extended diagnostics), add
them to `KNOWN_WINDOWS[]` in `tuya_codec.h` and update this table.

## 4. Register packing — 1 byte per register

Each Arctic "register" on the wire is **one byte**, not the two bytes
that classic Modbus uses. After the prefix is stripped (if any), byte
N of the payload corresponds to `reg_base + N`.

Signed temperature registers must be sign-extended from `int8_t` to
`int16_t` before being stored in any 16-bit register cache (see
`decode_byte()` in `modbus_sniffer.cpp` and `to_signed()` in
`arctic_registers.h`).

> **Known caveat — multi-byte status registers.** `sniff_cap3.jsonl`
> line 1 shows reg 2135 = `2355` (decimal) which is clearly a packed
> bitmap, not a single-byte temperature. The current decoder still
> treats every register as a single byte; multi-byte fields like 2135
> are a known limitation tracked in the migration plan and need
> per-register type metadata to decode correctly. Until that lands,
> only single-byte fields (temperatures, simple counts) can be trusted
> end-to-end.

## 5. Checksum

```
chk = ~sum(bytes[2 .. frame_len-2]) & 0xFF
    = (0xFF - (sum & 0xFF)) & 0xFF
```

The sum starts immediately after the `55 AA` magic and stops just
before the checksum byte. This matches the standard Tuya MCU spec.

Implementation: `tuya_codec::compute_checksum()`.

## 6. Scale factors

The scale model is documented in
[`main/arctic_registers.cpp`](../main/arctic_registers.cpp) lines 56–62
and was validated against real captures + on-site recall on 2026-05-03:

| Range           | Scale | Rationale |
|-----------------|-------|-----------|
| 2100 – 2116     | ÷2 signed | Sensor temperatures. Outdoor (2110) read raw=20, decoded 10 °C; matches the 5–10 °C the user saw on-site. |
| 2118 +          | per-register | Pressures / voltages / currents — see V1.3 PDF table. |
| 2000 – 2057     | ÷1 (raw)  | Holding regs. Setpoints would be nonsensical at ÷2 (DHW target = 110 °C). Reg 2003 = 45 (°C floor-heat target), reg 2004 = 55 (°C DHW target) — both sensible only as raw integers. |

## 7. Request / response pairing

Pairing is deterministic via the `dir` byte. The sniffer holds one
pending request and matches it to the next response with identical
`(fc, addr, count)`. Mismatches are counted as framing errors.

The MCU is **never** the initiator — it only ever sends responses, so
an unsolicited `dir=0x0F` is an error or a partial-frame artifact.

## 8. Things still unknown

1. **Write path.** Every capture is a status read. We do not yet have
   a packet capture of a setpoint write or an on/off toggle from the
   panel buttons. Once captured, decide whether to extend `KNOWN_WINDOWS`
   or introduce a `FC_WRITE` constant.
2. **Tuya MCU variant.** Captures look like the "basic" variant. Confirm
   against the Tuya MCU SDK docs before hard-coding any further frame
   types into the codec.
3. **Multi-byte status bitmaps.** See §4. Until per-register type
   metadata exists, registers like 2135 will be misdecoded by 1-byte
   consumers; raw payload is preserved on the wire, so no information
   is lost — only the decode step is incomplete.

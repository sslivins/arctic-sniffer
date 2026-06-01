# Tuya MCU Wire Protocol — Arctic Heat Pump

The Arctic heat pump's RS-485 panel link does **not** speak vanilla
Modbus RTU, despite what the V1.3 PDF implies. It speaks the **Tuya MCU
framing** used by the embedded Tuya Wi-Fi gateway module. This document
is the source of truth for the codec implemented in
[`main/tuya_codec.h`](../main/tuya_codec.h) /
[`main/tuya_codec.cpp`](../main/tuya_codec.cpp).

**Confidence key** used throughout this doc:

- ✅ **Validated** — confirmed against raw-byte captures (`tests/data/capture_raw.jsonl`
  and `arctic_capture_20260503-081652.jsonl`, both preserved as raw wire bytes
  in hex).
- ⚠️ **Tentative** — only seen in decoded-form captures
  (`arctic-simulator/captures/sniff_cap*.jsonl`). Those were produced by an
  earlier sniffer build whose output is internally inconsistent with the
  current 1-byte-per-reg framing (it contains values >255 for registers that
  raw-byte captures show are single-byte), so anything seen only there must
  be re-captured before it's trusted.
- ❓ **Unknown** — no capture evidence at all yet.

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

## 4. Register packing — 1 byte per register ✅

Each Arctic "register" on the wire is **one byte**, not the two bytes
that classic Modbus uses. After the prefix is stripped (if any), byte
N of the payload corresponds to `reg_base + N`. Confirmed by decoding
the raw `55 AA …` frames in `tests/data/capture_raw.jsonl`: telemetry
responses are exactly `count=50` bytes and yield exactly 50 reg
values, all within `0..255`.

Signed temperature/limit registers must be sign-extended from
`int8_t` to `int16_t` before being stored in any 16-bit register
cache (see `decode_byte()` in `modbus_sniffer.cpp` and `to_signed()`
in `arctic_registers.h`). Holding regs with raw values like 250, 246,
226 decode cleanly as −6, −10, −30 °C — sensible defrost/low-limit
thresholds.

> ⚠️ **Multi-byte status registers — UNVALIDATED.** Earlier notes in
> this doc claimed reg 2135 is a 16-bit bitmap based on
> `sniff_cap3.jsonl` line 1 showing `2135 = 2355`. That file was
> produced by an older decoder and contains values >255 for registers
> that raw-byte captures show as single bytes (e.g. 2122 = 1 raw, but
> 3200 in `sniff_cap.jsonl`). Raw-byte captures show **all of
> 2135–2138 = 0** in an idle, fault-free system, so we have no
> on-the-wire evidence either way for the bitmap layout. **Need a
> fresh raw capture with at least one fault active** before adding
> any multi-byte decoding logic.

## 5. Checksum

```
chk = ~sum(bytes[2 .. frame_len-2]) & 0xFF
    = (0xFF - (sum & 0xFF)) & 0xFF
```

The sum starts immediately after the `55 AA` magic and stops just
before the checksum byte. This matches the standard Tuya MCU spec.

Implementation: `tuya_codec::compute_checksum()`.

## 6. Scale factors

Verified against the 21 holding snapshots + 10 telemetry snapshots in
`tests/data/capture_raw.jsonl`, decoded fresh from raw wire bytes
and cross-checked against the user's on-site recall (5–10 °C
outdoor ambient, 2026-05-03).

| Range           | Scale | Confidence | Rationale |
|-----------------|-------|------------|-----------|
| 2100 – 2117 (temperatures) | ÷1 (raw, signed where applicable) | ✅ | Reg 2110 (outdoor coil) = 9 raw → 9 °C, fits 5–10 °C site recall. Reg 2108 (indoor coil) = 23 raw → 23 °C, normal room temp; would be implausibly cold at ÷2 (11.5 °C). |
| 2000 – 2057 (config/setpoints) | ÷1 (signed bytes for limits) | ✅ | Reg 2003 = 45 (floor-heat target °C), reg 2004 = 55 (DHW target °C), all sensible only raw. Signed-byte limits: 2013 = 250 → −6 °C, 2029 = 246 → −10 °C, 2038 = 226 → −30 °C, 2052 = 241 → −15 °C — all reasonable defrost / low-limit thresholds. |
| 2118 +          | per-register | ⚠️ | The V1.3 CSV claims pressures (÷10 bar) and voltages (÷10 V) in this range, but in the captured idle snapshots the only non-zero regs here are 2120=12, 2122=1, 2131=96 — too coarse to confirm or refute the ÷10 claim. Need a capture with the compressor running. |
| 2135 – 2138 (status bitmaps) | bitmap (layout TBD) | ⚠️ | CSV claims 16-bit bitmaps with named flags; all four regs are 0 in every raw-byte capture so far. **Need a fault-active capture.** |

**Explicitly rejected**: the previous draft of this doc claimed
"2100–2116 ÷2 signed". Raw-byte captures decoded fresh disagree — the
÷2 scaling was an artifact of looking at decoded JSONL from an older
sniffer build whose decoder is no longer trustworthy. Outdoor coil at
raw=9 maps to 9 °C, not 4.5 °C, and that's the value that fits both
the user's site recall **and** the indoor coil at room temperature.

## 7. Request / response pairing

Pairing is deterministic via the `dir` byte. The sniffer holds one
pending request and matches it to the next response with identical
`(fc, addr, count)`. Mismatches are counted as framing errors.

The MCU is **never** the initiator — it only ever sends responses, so
an unsolicited `dir=0x0F` is an error or a partial-frame artifact.

## 8. Things still unknown — capture before coding

Each item below needs a fresh **raw-byte** capture (the kind in
`tests/data/capture_raw.jsonl` with full `55 AA …` hex frames, not
the decoded-values JSONL form) before any controller-side code
should depend on it.

1. **Write function code.** ❓ The CSV claims FC=0x10 (Write
   Multiple) is supported. `sniff_cap.jsonl` (decoded form, ⚠️
   reliability) shows three FC=0x06 (Write Single) events writing
   regs 2000, 2003, 2037. Zero writes are present in raw-byte
   captures. **Press a panel button (on/off, setpoint up, mode
   change) during the next capture session** and confirm which FC
   the panel actually sends, what window/addr it targets, and the
   payload layout. Default the controller write path to FC=0x06
   single-reg until proven otherwise.
2. **Status-bitmap layout (2135 – 2138).** ⚠️ See §4 and §6. CSV
   has 30+ named flag bits across these four regs; raw captures
   show all zeros. **Need a capture with at least one fault active**
   — e.g., DHW probe disconnected, or a real running-state event.
3. **Pressure / voltage / current scaling (regs 2118+).** ⚠️ CSV
   suggests ÷10 for several of these; idle captures don't have
   enough signal to confirm or refute. **Need a capture with the
   compressor running** so AC mains (CSV: 2120), DC bus (CSV: 2122),
   compressor current (CSV: 2131) show their live values.
4. **Tuya MCU variant.** ❓ Captures look like the "basic" variant.
   Confirm against the Tuya MCU SDK docs before hard-coding any
   further frame types into the codec.
5. **Additional windows.** ❓ Only `(addr=0, count=50)` and
   `(addr=50, count=58)` have been observed in eight months. If new
   tuples appear (extended diagnostics, factory-mode reads), add
   them to `KNOWN_WINDOWS[]` rather than relaxing the validator.

### Capture procedure for the next session

1. Run the sniffer firmware from this repo's current `main`
   (post-1-byte-per-reg refactor) so the JSONL it writes is
   internally consistent with current decode assumptions.
2. **Always also save the raw `55 AA …` hex frames** (the format in
   `tests/data/capture_raw.jsonl`). Decoded JSONL alone is not
   enough — if decode assumptions change later, only raw frames let
   us re-derive ground truth.
3. Exercise the system: power on, change setpoint, trigger a known
   fault if possible, let the compressor run for at least a few
   minutes.
4. Drop the new capture into `arctic-simulator/tests/data/` and
   rerun the validation analysis before updating this doc.

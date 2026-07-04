#pragma once

// ---------------------------------------------------------------------------
// Tuya MCU framing codec for the Arctic heat pump wire protocol.
//
// Pure, dependency-free implementation: no ESP-IDF, no FreeRTOS, no UART.
// Designed to be linked into the sniffer (decode), the simulator (decode +
// encode responses), and the controller (encode requests + decode responses),
// AND to compile natively for unit tests on a host machine.
//
// Wire format (validated against captures, 2026-05-03):
//
//   55 AA <dir:1> <fc:1> <addr:2BE> <count:2BE> [data:count bytes] <chk:1>
//
//   dir   : 0xF0 = controller -> heat pump (request)
//           0x0F = heat pump -> controller (response)
//   fc    : 0x03 = read register block (only FC observed so far)
//   addr  : starting BYTE offset into the unified Arctic register page.
//           NOTE: this is NOT a Modbus register number; it is a byte offset.
//           Currently only two values seen: 0 (telemetry window) and 50
//           (holding window).
//   count : number of payload bytes in the response. Request frames carry
//           no payload (the request length is fixed at 9 bytes).
//   chk   : 1 byte. chk = ~sum(bytes_after_55AA_through_byte_before_chk) & 0xFF.
//
// Frame total length:
//   Request  = HDR_LEN + CHK_LEN              = 9 bytes
//   Response = HDR_LEN + count + CHK_LEN      = 9 + count bytes
//
// Two register windows are observed in rotation:
//
//   addr=0,  count=50 -> "telemetry" (input regs 2100+):
//                        bytes 0..6  = static 7-byte prefix
//                                      `0a 28 32 05 01 00 0f`
//                        byte 7      = reg 2100
//                        byte 8      = reg 2101
//                        ... etc up to byte 49 = reg 2142
//
//   addr=50, count=58 -> "holding" (regs 2000..2057):
//                        no prefix; byte 0 = reg 2000, byte 1 = reg 2001, ...
//
// Each Arctic "register" on the wire is 1 byte (NOT 2 bytes like classic
// Modbus). Per-register scaling and signedness are defined elsewhere
// (see arctic_registers.cpp).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>

namespace tuya_codec {

// ---------------------------------------------------------------------------
// Frame constants
// ---------------------------------------------------------------------------

constexpr uint8_t HDR0          = 0x55;
constexpr uint8_t HDR1          = 0xAA;
constexpr uint8_t DIR_REQUEST   = 0xF0;
constexpr uint8_t DIR_RESPONSE  = 0x0F;
constexpr uint8_t FC_READ       = 0x03;
constexpr uint8_t FC_CMD        = 0x06;  // controller command (power/mode/setpoint)

constexpr size_t  HDR_LEN       = 8;    // 55 AA dir fc addr:2 count:2
constexpr size_t  CHK_LEN       = 1;
constexpr size_t  MIN_FRAME_LEN = HDR_LEN + CHK_LEN;   // 9 (request)
constexpr size_t  MAX_FRAME_LEN = 256;

// ---------------------------------------------------------------------------
// Register windows: maps the wire's (addr,count) pair to an Arctic register
// base address and a byte prefix to skip.
// ---------------------------------------------------------------------------

struct RegWindow {
    uint16_t field_a;     // wire `addr` field
    uint16_t field_b;     // wire `count` field
    uint16_t reg_base;    // first Arctic register number this window carries
    uint8_t  prefix_len;  // payload bytes to skip before register data starts
};

constexpr RegWindow KNOWN_WINDOWS[] = {
    { 0,  50, 2100, 7 },  // telemetry (input regs)
    { 50, 58, 2000, 0 },  // holding regs
};
constexpr size_t KNOWN_WINDOWS_COUNT = sizeof(KNOWN_WINDOWS) / sizeof(KNOWN_WINDOWS[0]);

/// Look up the window for a given (addr, count) tuple. Returns nullptr if
/// the tuple does not correspond to a recognised window.
const RegWindow *find_window(uint16_t field_a, uint16_t field_b);

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------

/// Compute the 1-byte Tuya checksum over a complete frame buffer.
/// Sums bytes [2 .. frame_len - 2] (i.e. everything after the 55 AA magic,
/// up to but not including the checksum slot), then returns ~sum & 0xFF.
uint8_t compute_checksum(const uint8_t *frame, size_t frame_len);

// ---------------------------------------------------------------------------
// Frame length
// ---------------------------------------------------------------------------

/// Compute the expected total frame length given the dir byte and the
/// count field (`field_b`). Returns 0 if dir is unknown or the length
/// would exceed MAX_FRAME_LEN.
size_t frame_total_len(uint8_t dir, uint16_t field_b);

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

enum class ParseResult {
    OK,
    BAD_MAGIC,         // first two bytes are not 55 AA
    TRUNCATED,         // buf is shorter than the declared frame length
    BAD_DIR,           // dir is not 0xF0 or 0x0F
    BAD_FC,            // fc is not 0x03
    UNKNOWN_WINDOW,    // (addr, count) tuple is not in KNOWN_WINDOWS
    BAD_CHECKSUM,      // computed checksum does not match the trailing byte
};

struct ParsedFrame {
    uint8_t        dir;          // DIR_REQUEST or DIR_RESPONSE
    uint8_t        fc;           // function code (currently always FC_READ)
    uint16_t       field_a;      // wire addr field (byte offset)
    uint16_t       field_b;      // wire count field (payload byte count)
    const RegWindow *window;     // matched window (never null on OK)
    const uint8_t *payload;      // pointer to first payload byte, or nullptr
                                 // for request frames (which carry no payload)
    size_t         payload_len;  // = field_b for responses, 0 for requests
    size_t         frame_len;    // total frame length consumed from buf
    uint8_t        checksum;     // received checksum byte
};

/// Parse a single Tuya frame from `buf` (which must start at the 55 AA magic).
/// On OK, fills `out` and returns ParseResult::OK with out.frame_len bytes
/// consumed. On any error, `out` is left in an unspecified state.
///
/// `buf_len` is the number of bytes available in buf. If buf_len is less
/// than the declared frame length, returns TRUNCATED (the caller should
/// wait for more bytes before retrying).
ParseResult parse_frame(const uint8_t *buf, size_t buf_len, ParsedFrame &out);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

/// Encode a request frame (9 bytes: 55 AA F0 fc addr:2 count:2 chk).
/// Returns the number of bytes written, or 0 on error
/// (buf_capacity < MIN_FRAME_LEN, unknown window).
size_t encode_request(uint8_t *buf, size_t buf_capacity,
                      uint8_t fc, uint16_t field_a, uint16_t field_b);

/// Encode a response frame (HDR_LEN + field_b + CHK_LEN bytes).
/// `payload` must point to exactly `field_b` bytes, which are copied
/// verbatim into the frame. Returns the number of bytes written, or 0
/// on error (buf_capacity too small, unknown window, payload null when
/// field_b > 0).
size_t encode_response(uint8_t *buf, size_t buf_capacity,
                       uint8_t fc, uint16_t field_a, uint16_t field_b,
                       const uint8_t *payload);

// ---------------------------------------------------------------------------
// Frame discovery (for streaming byte sources)
// ---------------------------------------------------------------------------

/// Find the first plausible frame-start offset in `buf`. A plausible start
/// is `55 AA` followed by a header that passes a basic sanity check
/// (known dir, known fc, known window). Returns `buf_len` if no candidate
/// is found (caller should keep accumulating bytes).
///
/// Use this with a streaming UART loop: scan for a plausible start, then
/// call `parse_frame()` to validate the checksum and extract the payload.
size_t find_frame_start(const uint8_t *buf, size_t buf_len);

}  // namespace tuya_codec

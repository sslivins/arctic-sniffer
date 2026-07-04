#include "tuya_codec.h"

#include <cstring>

namespace tuya_codec {

// ---------------------------------------------------------------------------
// Window lookup
// ---------------------------------------------------------------------------

const RegWindow *find_window(uint16_t field_a, uint16_t field_b)
{
    for (size_t i = 0; i < KNOWN_WINDOWS_COUNT; ++i) {
        const RegWindow &w = KNOWN_WINDOWS[i];
        if (w.field_a == field_a && w.field_b == field_b) {
            return &w;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------

uint8_t compute_checksum(const uint8_t *frame, size_t frame_len)
{
    if (frame_len < MIN_FRAME_LEN) return 0;
    uint32_t s = 0;
    // Sum from byte 2 (skip 55 AA) up to but not including the trailing chk.
    for (size_t i = 2; i + 1 < frame_len; ++i) {
        s += frame[i];
    }
    return static_cast<uint8_t>((0xFF - (s & 0xFF)) & 0xFF);
}

// ---------------------------------------------------------------------------
// Frame length
// ---------------------------------------------------------------------------

size_t frame_total_len(uint8_t dir, uint16_t field_b)
{
    size_t flen = 0;
    if (dir == DIR_REQUEST) {
        flen = HDR_LEN + CHK_LEN;             // 9
    } else if (dir == DIR_RESPONSE) {
        flen = HDR_LEN + field_b + CHK_LEN;   // 9 + count
    } else {
        return 0;
    }
    return (flen > MAX_FRAME_LEN) ? 0 : flen;
}

// ---------------------------------------------------------------------------
// Header sanity (private helper)
// ---------------------------------------------------------------------------

namespace {

bool header_dir_ok(uint8_t dir)
{
    return dir == DIR_REQUEST || dir == DIR_RESPONSE;
}

bool header_fc_ok(uint8_t fc)
{
    return fc == FC_READ || fc == FC_CMD;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

ParseResult parse_frame(const uint8_t *buf, size_t buf_len, ParsedFrame &out)
{
    if (buf_len < HDR_LEN) {
        return ParseResult::TRUNCATED;
    }
    if (buf[0] != HDR0 || buf[1] != HDR1) {
        return ParseResult::BAD_MAGIC;
    }

    const uint8_t  dir = buf[2];
    const uint8_t  fc  = buf[3];
    const uint16_t a   = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
    const uint16_t b   = static_cast<uint16_t>((buf[6] << 8) | buf[7]);

    if (!header_dir_ok(dir)) return ParseResult::BAD_DIR;
    if (!header_fc_ok(fc))   return ParseResult::BAD_FC;

    const RegWindow *win = nullptr;
    size_t flen = 0;
    if (fc == FC_CMD) {
        // Command frame: field_a/field_b are a command selector/value, not a
        // register window. Fixed 9-byte length in both directions (the value
        // is inline in field_b; there is no separate payload).
        flen = HDR_LEN + CHK_LEN;
    } else {
        win = find_window(a, b);
        if (!win) return ParseResult::UNKNOWN_WINDOW;
        flen = frame_total_len(dir, b);
        if (flen == 0 || flen > MAX_FRAME_LEN) {
            return ParseResult::UNKNOWN_WINDOW;  // implausible header
        }
    }
    if (buf_len < flen) {
        return ParseResult::TRUNCATED;
    }

    const uint8_t chk_actual   = buf[flen - 1];
    const uint8_t chk_expected = compute_checksum(buf, flen);
    if (chk_actual != chk_expected) {
        return ParseResult::BAD_CHECKSUM;
    }

    out.dir         = dir;
    out.fc          = fc;
    out.field_a     = a;
    out.field_b     = b;
    out.window      = win;
    out.frame_len   = flen;
    out.checksum    = chk_actual;
    if (fc == FC_READ && dir == DIR_RESPONSE) {
        out.payload     = buf + HDR_LEN;
        out.payload_len = b;
    } else {
        out.payload     = nullptr;
        out.payload_len = 0;
    }
    return ParseResult::OK;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace {

void write_header(uint8_t *buf, uint8_t dir, uint8_t fc,
                  uint16_t field_a, uint16_t field_b)
{
    buf[0] = HDR0;
    buf[1] = HDR1;
    buf[2] = dir;
    buf[3] = fc;
    buf[4] = static_cast<uint8_t>((field_a >> 8) & 0xFF);
    buf[5] = static_cast<uint8_t>(field_a & 0xFF);
    buf[6] = static_cast<uint8_t>((field_b >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>(field_b & 0xFF);
}

}  // namespace

size_t encode_request(uint8_t *buf, size_t buf_capacity,
                      uint8_t fc, uint16_t field_a, uint16_t field_b)
{
    if (!buf) return 0;
    if (buf_capacity < MIN_FRAME_LEN) return 0;
    if (!header_fc_ok(fc)) return 0;
    if (!find_window(field_a, field_b)) return 0;

    write_header(buf, DIR_REQUEST, fc, field_a, field_b);
    buf[HDR_LEN] = compute_checksum(buf, MIN_FRAME_LEN);
    return MIN_FRAME_LEN;
}

size_t encode_response(uint8_t *buf, size_t buf_capacity,
                       uint8_t fc, uint16_t field_a, uint16_t field_b,
                       const uint8_t *payload)
{
    if (!buf) return 0;
    if (!header_fc_ok(fc)) return 0;
    if (!find_window(field_a, field_b)) return 0;
    if (field_b > 0 && !payload) return 0;

    const size_t flen = frame_total_len(DIR_RESPONSE, field_b);
    if (flen == 0 || flen > MAX_FRAME_LEN) return 0;
    if (buf_capacity < flen) return 0;

    write_header(buf, DIR_RESPONSE, fc, field_a, field_b);
    if (field_b > 0) {
        std::memcpy(buf + HDR_LEN, payload, field_b);
    }
    buf[flen - 1] = compute_checksum(buf, flen);
    return flen;
}

// ---------------------------------------------------------------------------
// Frame discovery
// ---------------------------------------------------------------------------

size_t find_frame_start(const uint8_t *buf, size_t buf_len)
{
    if (buf_len < HDR_LEN) return buf_len;

    for (size_t i = 0; i + HDR_LEN <= buf_len; ++i) {
        if (buf[i] != HDR0)     continue;
        if (buf[i + 1] != HDR1) continue;

        const uint8_t  dir = buf[i + 2];
        const uint8_t  fc  = buf[i + 3];
        const uint16_t a   = static_cast<uint16_t>((buf[i + 4] << 8) | buf[i + 5]);
        const uint16_t b   = static_cast<uint16_t>((buf[i + 6] << 8) | buf[i + 7]);

        if (!header_dir_ok(dir)) continue;
        if (!header_fc_ok(fc))   continue;
        if (fc != FC_CMD && !find_window(a, b)) continue;

        return i;
    }
    return buf_len;
}

}  // namespace tuya_codec

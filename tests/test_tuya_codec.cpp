// Native-build unit tests for tuya_codec. No ESP-IDF, no FreeRTOS, no
// external deps — just standard C++17. Compile with the host toolchain.
//
// Build & run:
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//
// main() returns 0 if every assertion passes, non-zero otherwise.

#include "tuya_codec.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
const char *g_current_test = "";

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++g_pass; }                                              \
        else {                                                               \
            ++g_fail;                                                        \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                    \
                         g_current_test, __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define TEST(name) g_current_test = name

// Decode a hex string ("55aa...") into a byte buffer.
std::vector<uint8_t> hex(const char *s)
{
    std::vector<uint8_t> out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t n = std::strlen(s);
    for (size_t i = 0; i + 1 < n; i += 2) {
        int hi = nibble(s[i]);
        int lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_constants()
{
    TEST("constants");
    CHECK(tuya_codec::HDR0 == 0x55);
    CHECK(tuya_codec::HDR1 == 0xAA);
    CHECK(tuya_codec::DIR_REQUEST == 0xF0);
    CHECK(tuya_codec::DIR_RESPONSE == 0x0F);
    CHECK(tuya_codec::FC_READ == 0x03);
    CHECK(tuya_codec::HDR_LEN == 8);
    CHECK(tuya_codec::CHK_LEN == 1);
    CHECK(tuya_codec::MIN_FRAME_LEN == 9);
    CHECK(tuya_codec::KNOWN_WINDOWS_COUNT == 2);
}

void test_find_window()
{
    TEST("find_window");
    auto *w0 = tuya_codec::find_window(0, 50);
    CHECK(w0 != nullptr);
    CHECK(w0->reg_base == 2100);
    CHECK(w0->prefix_len == 7);

    auto *w1 = tuya_codec::find_window(50, 58);
    CHECK(w1 != nullptr);
    CHECK(w1->reg_base == 2000);
    CHECK(w1->prefix_len == 0);

    CHECK(tuya_codec::find_window(0, 0) == nullptr);
    CHECK(tuya_codec::find_window(99, 99) == nullptr);
    CHECK(tuya_codec::find_window(50, 50) == nullptr);
}

void test_frame_total_len()
{
    TEST("frame_total_len");
    CHECK(tuya_codec::frame_total_len(tuya_codec::DIR_REQUEST, 0)  == 9);
    CHECK(tuya_codec::frame_total_len(tuya_codec::DIR_REQUEST, 50) == 9);
    CHECK(tuya_codec::frame_total_len(tuya_codec::DIR_RESPONSE, 0)  == 9);
    CHECK(tuya_codec::frame_total_len(tuya_codec::DIR_RESPONSE, 50) == 59);
    CHECK(tuya_codec::frame_total_len(tuya_codec::DIR_RESPONSE, 58) == 67);
    CHECK(tuya_codec::frame_total_len(0xAB, 50) == 0);  // bad dir
}

void test_compute_checksum_known()
{
    TEST("compute_checksum_known");
    // Real request from capture line 1: 55 AA F0 03 00 32 00 3A A0
    auto req = hex("55aaf0030032003aa0");
    CHECK(req.size() == 9);
    uint8_t chk = tuya_codec::compute_checksum(req.data(), req.size());
    CHECK(chk == 0xA0);
}

void test_parse_request_real_capture()
{
    TEST("parse_request_real_capture");
    auto req = hex("55aaf0030032003aa0");
    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(req.data(), req.size(), pf);
    CHECK(pr == tuya_codec::ParseResult::OK);
    CHECK(pf.dir == tuya_codec::DIR_REQUEST);
    CHECK(pf.fc  == tuya_codec::FC_READ);
    CHECK(pf.field_a == 50);
    CHECK(pf.field_b == 58);
    CHECK(pf.payload == nullptr);
    CHECK(pf.payload_len == 0);
    CHECK(pf.frame_len == 9);
    CHECK(pf.window != nullptr);
    CHECK(pf.window->reg_base == 2000);
}

void test_parse_response_real_capture()
{
    TEST("parse_response_real_capture");
    // Response half of capture line 1 (A=50, B=58). The captured wire bytes
    // include one trailing idle byte after the chk, so the buffer is 68 bytes
    // long but the frame itself is 67. parse_frame must consume only frame_len.
    auto resp = hex(
        "55aa0f030032003a002000000000002014232d2d32fa030000000000042d001c1205"
        "050000f63cf90a2d050c5f0fe232ff0a0a050205ff0000000014f10000000008f800");
    CHECK(resp.size() == 68);

    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(resp.data(), resp.size(), pf);
    CHECK(pr == tuya_codec::ParseResult::OK);
    if (pr != tuya_codec::ParseResult::OK) return;

    CHECK(pf.dir == tuya_codec::DIR_RESPONSE);
    CHECK(pf.field_a == 50);
    CHECK(pf.field_b == 58);
    CHECK(pf.payload != nullptr);
    CHECK(pf.payload_len == 58);
    CHECK(pf.frame_len == 67);
    // First payload byte mirrors header offset 8 of the captured frame (0x00).
    CHECK(pf.payload[0] == 0x00);
    // Last payload byte is at buffer offset 65 (0x08); 0xF8 is the chk byte.
    CHECK(pf.payload[57] == 0x08);
}

void test_parse_response_telemetry_window()
{
    TEST("parse_response_telemetry_window");
    // Response half of capture line 3 (A=0, B=50). Captured length is 60 bytes
    // (1 trailing idle byte); frame itself is 59. Validates the (0,50) window
    // and the 7-byte static prefix.
    auto resp = hex(
        "55aa0f03000000320a28320501000f1e17060911230020231e2800000c0001000000"
        "000000000060000000000000000a0a0f0b0b0f0e001300e68514");
    CHECK(resp.size() == 60);

    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(resp.data(), resp.size(), pf);
    CHECK(pr == tuya_codec::ParseResult::OK);
    if (pr != tuya_codec::ParseResult::OK) return;

    CHECK(pf.field_a == 0);
    CHECK(pf.field_b == 50);
    CHECK(pf.payload != nullptr);
    CHECK(pf.payload_len == 50);
    CHECK(pf.frame_len == 59);
    // Static 7-byte prefix: 0a 28 32 05 01 00 0f
    CHECK(pf.payload[0] == 0x0A);
    CHECK(pf.payload[1] == 0x28);
    CHECK(pf.payload[2] == 0x32);
    CHECK(pf.payload[3] == 0x05);
    CHECK(pf.payload[4] == 0x01);
    CHECK(pf.payload[5] == 0x00);
    CHECK(pf.payload[6] == 0x0F);
}

void test_parse_truncated()
{
    TEST("parse_truncated");
    auto req = hex("55aaf0030032003aa0");
    tuya_codec::ParsedFrame pf{};
    for (size_t n = 0; n < req.size(); ++n) {
        auto pr = tuya_codec::parse_frame(req.data(), n, pf);
        CHECK(pr == tuya_codec::ParseResult::TRUNCATED);
    }
}

void test_parse_bad_magic()
{
    TEST("parse_bad_magic");
    uint8_t bad[] = {0x56, 0xAA, 0xF0, 0x03, 0x00, 0x32, 0x00, 0x3A, 0xA0};
    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(bad, sizeof(bad), pf)
          == tuya_codec::ParseResult::BAD_MAGIC);
    bad[0] = 0x55; bad[1] = 0xAB;
    CHECK(tuya_codec::parse_frame(bad, sizeof(bad), pf)
          == tuya_codec::ParseResult::BAD_MAGIC);
}

void test_parse_bad_dir()
{
    TEST("parse_bad_dir");
    uint8_t bad[] = {0x55, 0xAA, 0xAB, 0x03, 0x00, 0x32, 0x00, 0x3A, 0x00};
    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(bad, sizeof(bad), pf)
          == tuya_codec::ParseResult::BAD_DIR);
}

void test_parse_bad_fc()
{
    TEST("parse_bad_fc");
    // 0x99 is neither FC_READ (0x03) nor FC_CMD (0x06); both of those are valid.
    uint8_t bad[] = {0x55, 0xAA, 0xF0, 0x99, 0x00, 0x32, 0x00, 0x3A, 0x00};
    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(bad, sizeof(bad), pf)
          == tuya_codec::ParseResult::BAD_FC);
}

void test_parse_unknown_window()
{
    TEST("parse_unknown_window");
    // Well-formed request (valid checksum 0xDC) for window (16, 32), which is
    // not in KNOWN_WINDOWS. parse_frame is a superset: it validates framing +
    // checksum first, then returns UNKNOWN_WINDOW for the unmapped tuple.
    uint8_t bad[] = {0x55, 0xAA, 0xF0, 0x03, 0x00, 0x10, 0x00, 0x20, 0xDC};
    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(bad, sizeof(bad), pf)
          == tuya_codec::ParseResult::UNKNOWN_WINDOW);
}

void test_parse_bad_checksum()
{
    TEST("parse_bad_checksum");
    auto req = hex("55aaf0030032003aa0");
    req.back() ^= 0x01;  // corrupt chk
    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(req.data(), req.size(), pf)
          == tuya_codec::ParseResult::BAD_CHECKSUM);
}

void test_encode_request_roundtrip()
{
    TEST("encode_request_roundtrip");
    uint8_t buf[16] = {};
    size_t n = tuya_codec::encode_request(buf, sizeof(buf),
                                          tuya_codec::FC_READ, 50, 58);
    CHECK(n == 9);
    CHECK(buf[8] == 0xA0);  // expected checksum matches the real capture

    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(buf, n, pf) == tuya_codec::ParseResult::OK);
    CHECK(pf.dir == tuya_codec::DIR_REQUEST);
    CHECK(pf.field_a == 50);
    CHECK(pf.field_b == 58);
}

void test_encode_request_rejects_bad_inputs()
{
    TEST("encode_request_rejects_bad_inputs");
    uint8_t buf[16] = {};
    CHECK(tuya_codec::encode_request(nullptr, 16, tuya_codec::FC_READ, 50, 58) == 0);
    CHECK(tuya_codec::encode_request(buf, 4, tuya_codec::FC_READ, 50, 58) == 0);
    CHECK(tuya_codec::encode_request(buf, 16, 0x99, 50, 58) == 0);             // bad fc
    CHECK(tuya_codec::encode_request(buf, 16, tuya_codec::FC_READ, 1, 1) == 0); // bad window
}

void test_encode_response_roundtrip()
{
    TEST("encode_response_roundtrip");
    uint8_t payload[58] = {};
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = static_cast<uint8_t>(i);

    uint8_t buf[128] = {};
    size_t n = tuya_codec::encode_response(buf, sizeof(buf),
                                           tuya_codec::FC_READ, 50, 58, payload);
    CHECK(n == 67);

    tuya_codec::ParsedFrame pf{};
    CHECK(tuya_codec::parse_frame(buf, n, pf) == tuya_codec::ParseResult::OK);
    CHECK(pf.dir == tuya_codec::DIR_RESPONSE);
    CHECK(pf.payload_len == 58);
    CHECK(std::memcmp(pf.payload, payload, sizeof(payload)) == 0);
}

void test_encode_response_rejects_bad_inputs()
{
    TEST("encode_response_rejects_bad_inputs");
    uint8_t buf[128] = {};
    uint8_t payload[58] = {};
    // Buffer too small.
    CHECK(tuya_codec::encode_response(buf, 10, tuya_codec::FC_READ, 50, 58, payload) == 0);
    // Null payload with nonzero count.
    CHECK(tuya_codec::encode_response(buf, 128, tuya_codec::FC_READ, 50, 58, nullptr) == 0);
    // Bad window.
    CHECK(tuya_codec::encode_response(buf, 128, tuya_codec::FC_READ, 1, 1, payload) == 0);
}

void test_find_frame_start_clean()
{
    TEST("find_frame_start_clean");
    auto req = hex("55aaf0030032003aa0");
    CHECK(tuya_codec::find_frame_start(req.data(), req.size()) == 0);
}

void test_find_frame_start_with_garbage_prefix()
{
    TEST("find_frame_start_with_garbage_prefix");
    std::vector<uint8_t> buf = {0xDE, 0xAD, 0xBE, 0xEF};
    auto req = hex("55aaf0030032003aa0");
    buf.insert(buf.end(), req.begin(), req.end());
    CHECK(tuya_codec::find_frame_start(buf.data(), buf.size()) == 4);
}

void test_find_frame_start_skips_bad_header()
{
    TEST("find_frame_start_skips_bad_header");
    // 55 AA at offset 0 but FC=0x99 (invalid) — should be skipped, real frame
    // at offset 9. (0x06/FC_CMD is a *valid* fc, so it cannot be used here.)
    std::vector<uint8_t> buf =
        {0x55, 0xAA, 0xF0, 0x99, 0x00, 0x32, 0x00, 0x3A, 0x00};
    auto req = hex("55aaf0030032003aa0");
    buf.insert(buf.end(), req.begin(), req.end());
    CHECK(tuya_codec::find_frame_start(buf.data(), buf.size()) == 9);
}

void test_find_frame_start_no_frame()
{
    TEST("find_frame_start_no_frame");
    std::vector<uint8_t> buf =
        {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00};
    CHECK(tuya_codec::find_frame_start(buf.data(), buf.size()) == buf.size());
}

}  // namespace

int main()
{
    test_constants();
    test_find_window();
    test_frame_total_len();
    test_compute_checksum_known();
    test_parse_request_real_capture();
    test_parse_response_real_capture();
    test_parse_response_telemetry_window();
    test_parse_truncated();
    test_parse_bad_magic();
    test_parse_bad_dir();
    test_parse_bad_fc();
    test_parse_unknown_window();
    test_parse_bad_checksum();
    test_encode_request_roundtrip();
    test_encode_request_rejects_bad_inputs();
    test_encode_response_roundtrip();
    test_encode_response_rejects_bad_inputs();
    test_find_frame_start_clean();
    test_find_frame_start_with_garbage_prefix();
    test_find_frame_start_skips_bad_header();
    test_find_frame_start_no_frame();

    std::printf("tuya_codec tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

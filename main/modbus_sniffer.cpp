#include "modbus_sniffer.h"
#include "macon_registers.h"
#include "recorder.h"
#include "tuya_codec.h"

#include <cstring>
#include <atomic>
#include <sys/time.h>

#include "driver/uart.h"
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sniffer";

namespace sniffer {

// ---------------------------------------------------------------------------
// Tuya-framed Arctic protocol — wire format constants and codec live in
// tuya_codec.{h,cpp}. This file is the sniffer integration: UART driver,
// streaming reassembly, and request/response pairing.
// ---------------------------------------------------------------------------

constexpr size_t  MAX_BLOB     = 512;   // max bytes in one UART batch
constexpr int     UART_BUF_SZ  = 1024;
constexpr int     UART_QUEUE_SZ = 20;
constexpr uint8_t RX_TOUT_THRESH = 40;  // hardware TOUT in bit-times

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static TransactionCallback s_callback;
static std::atomic<uint32_t> s_frame_count{0};
// Genuinely malformed / unpaired frames: parse or checksum failures, insane
// lengths, and orphan responses. This is what was historically (and
// misleadingly) surfaced as "crc_errors" even though the RS-485 bus is clean
// and frame checksums pass — in practice this stays at ~0.
static std::atomic<uint32_t> s_parse_errors{0};
// Benign inter-frame bytes discarded during resync — e.g. the half-duplex
// line-turnaround byte between a response and the next request. NOT errors;
// see the s_skipped_val capture below for what they actually are.
static std::atomic<uint32_t> s_resync_bytes{0};
static std::atomic<uint32_t> s_txn_count{0};

// Ring buffer capturing the actual raw bytes we discard during resync, so we
// can identify what the stray inter-frame bytes really are (value + capture
// time). Newest entries overwrite oldest once full.
constexpr size_t SKIPPED_RING_SZ = 256;
static uint8_t   s_skipped_val[SKIPPED_RING_SZ];
static int64_t   s_skipped_ms[SKIPPED_RING_SZ];
// Per-byte correlation context: the length / dir / fc of the last VALID frame
// consumed immediately BEFORE each skipped byte. This lets /api/skipped test
// whether a stray value (e.g. the recurring 0x14) tracks the preceding frame's
// byte length (→ a byte-count ack) or stays constant (→ a fixed heartbeat).
static uint16_t  s_skipped_prevlen[SKIPPED_RING_SZ];
static uint8_t   s_skipped_prevdir[SKIPPED_RING_SZ];
static uint8_t   s_skipped_prevfc[SKIPPED_RING_SZ];
static size_t    s_skipped_head  = 0;   // next write slot
static uint32_t  s_skipped_total = 0;   // total bytes ever captured

// Context of the most recently consumed valid frame, attached to skipped bytes.
static uint16_t  s_last_frame_len = 0;
static uint8_t   s_last_frame_dir = 0;
static uint8_t   s_last_frame_fc  = 0;

/// Zero all live counters and the skipped-byte capture (used on init and
/// whenever UART parameters change to give a clean baseline).
static void reset_counters()
{
    s_frame_count.store(0, std::memory_order_relaxed);
    s_parse_errors.store(0, std::memory_order_relaxed);
    s_resync_bytes.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
    s_skipped_head  = 0;
    s_skipped_total = 0;
    s_last_frame_len = 0;
    s_last_frame_dir = 0;
    s_last_frame_fc  = 0;
}

// UART event queue — filled by the driver ISR
static QueueHandle_t s_uart_queue = nullptr;

// Accumulation buffer — collects bytes until TOUT, then split into frames
static uint8_t  s_blob_buf[MAX_BLOB];
static size_t   s_blob_len = 0;

// Pending request (waiting for matching response). Pairing is now
// deterministic via the dir byte: requests carry dir=0xF0, responses
// dir=0x0F. We hold a request while waiting for a matching response
// (same fc / fieldA / fieldB).
static bool         s_have_pending = false;
static Transaction  s_pending;
static uint16_t     s_pending_field_a = 0;
static uint16_t     s_pending_field_b = 0;
static int64_t      s_pending_uptime_ms = 0;

// ---------------------------------------------------------------------------
// Reverse-engineering aids: raw register snapshot + fc=0x06 command ring
// ---------------------------------------------------------------------------

// Raw (undecoded) latest byte per register. Covers 2000..2199 which spans
// both the holding (2000..2057) and telemetry (2100..2142) windows.
constexpr uint16_t SNAP_BASE = 2000;
constexpr uint16_t SNAP_SPAN = 200;
static uint8_t  s_snap_raw[SNAP_SPAN]   = {0};
static bool     s_snap_valid[SNAP_SPAN] = {false};

static CommandRec s_cmd_ring[COMMAND_RING_SZ];
static std::atomic<uint32_t> s_cmd_count{0};   // total fc=0x06 frames seen

static int64_t now_ms();  // defined below

static void snapshot_store(uint16_t addr, uint8_t raw)
{
    if (addr < SNAP_BASE) return;
    uint16_t idx = (uint16_t)(addr - SNAP_BASE);
    if (idx >= SNAP_SPAN) return;
    s_snap_raw[idx]   = raw;
    s_snap_valid[idx] = true;
}

// ---------------------------------------------------------------------------
// Unknown-register tracker — always-on capture of any address the decoder has
// no metadata for (arctic::register_lookup() == nullptr). Survives snapshot
// clears so unexpected registers appearing while the device is left unattended
// are recorded for later review. Indexed over the same 2000..2199 span.
// ---------------------------------------------------------------------------
static uint8_t  s_unk_raw[SNAP_SPAN]     = {0};
static bool     s_unk_valid[SNAP_SPAN]   = {false};
static uint32_t s_unk_seen[SNAP_SPAN]    = {0};
static uint32_t s_unk_changes[SNAP_SPAN] = {0};
static int64_t  s_unk_first[SNAP_SPAN]   = {0};
static int64_t  s_unk_last[SNAP_SPAN]    = {0};
static std::atomic<uint32_t> s_unk_count{0};  // distinct unknown addrs seen

static void track_unknown(uint16_t addr, uint8_t raw)
{
    // Only track addresses the decoder doesn't recognize.
    if (arctic::register_lookup(addr) != nullptr) return;

    if (addr < SNAP_BASE || (uint16_t)(addr - SNAP_BASE) >= SNAP_SPAN) {
        // Out-of-span unknown address — genuinely unexpected. Log it (the
        // per-address table only covers the known span).
        ESP_LOGW(TAG, "Unknown register 0x%04X (%u) out of tracking span: raw=0x%02X",
                 addr, addr, raw);
        return;
    }

    uint16_t idx = (uint16_t)(addr - SNAP_BASE);
    int64_t now = now_ms();

    if (!s_unk_valid[idx]) {
        s_unk_valid[idx]  = true;
        s_unk_raw[idx]    = raw;
        s_unk_seen[idx]   = 1;
        s_unk_changes[idx]= 0;
        s_unk_first[idx]  = now;
        s_unk_last[idx]   = now;
        s_unk_count.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG, "Unknown register %u first seen: raw=0x%02X", addr, raw);
        return;
    }

    s_unk_seen[idx]++;
    s_unk_last[idx] = now;
    if (s_unk_raw[idx] != raw) {
        ESP_LOGW(TAG, "Unknown register %u changed: 0x%02X -> 0x%02X",
                 addr, s_unk_raw[idx], raw);
        s_unk_raw[idx] = raw;
        s_unk_changes[idx]++;
    }
}

static void record_command(uint8_t dir, uint16_t selector, uint16_t value)
{
    uint32_t n = s_cmd_count.fetch_add(1, std::memory_order_relaxed);
    CommandRec &rec = s_cmd_ring[n % COMMAND_RING_SZ];
    rec.timestamp_ms = now_ms();
    rec.dir          = dir;
    rec.selector     = selector;
    rec.value        = value;
}

// ---------------------------------------------------------------------------
// Diagnostic hex dump
// ---------------------------------------------------------------------------

static void log_hex(const char *prefix, const uint8_t *data, size_t len)
{
    constexpr size_t MAX_SHOW = 20;
    char hex[MAX_SHOW * 3 + 1];
    size_t n = (len < MAX_SHOW) ? len : MAX_SHOW;
    for (size_t i = 0; i < n; i++) {
        snprintf(hex + i * 3, 4, "%02X ", data[i]);
    }
    if (n > 0) hex[n * 3 - 1] = '\0';
    else hex[0] = '\0';
    ESP_LOGD(TAG, "%s (%u bytes): %s%s", prefix, (unsigned)len, hex,
             (len > MAX_SHOW) ? "..." : "");
}

// ---------------------------------------------------------------------------
// Transaction emission helpers
// ---------------------------------------------------------------------------

/// Get a sane "now" in ms — wall clock if NTP synced, else uptime.
static int64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1577836800) {
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    return esp_timer_get_time() / 1000;
}

/// Decode a single response byte into a Transaction value, applying 8-bit
/// sign extension when the register is signed so that the formatter's
/// 16-bit `to_signed()` produces the right number.
static uint16_t decode_byte(uint8_t raw, uint16_t reg_addr)
{
    const arctic::RegisterInfo *ri = arctic::register_lookup(reg_addr);
    if (ri && ri->is_signed) {
        // sign-extend int8 -> int16 -> uint16
        return (uint16_t)(int16_t)(int8_t)raw;
    }
    return (uint16_t)raw;
}

/// Process a complete, structurally validated Tuya frame.
static void process_frame(const uint8_t *buf, size_t len)
{
    tuya_codec::ParsedFrame pf{};
    auto pr = tuya_codec::parse_frame(buf, len, pf);
    if (pr != tuya_codec::ParseResult::OK) {
        ESP_LOGW(TAG, "Frame parse failed: code=%d", (int)pr);
        s_parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint8_t  dir = pf.dir;
    const uint8_t  fc  = pf.fc;
    const uint16_t a   = pf.field_a;
    const uint16_t b   = pf.field_b;
    const tuya_codec::RegWindow *win = pf.window;

    // fc=0x06 command frame (power/mode/setpoint). No register window; the
    // (a,b) pair is a command selector/value. Record it and return — do NOT
    // run it through the read request/response pairing below.
    if (fc == tuya_codec::FC_CMD) {
        record_command(dir, a, b);
        return;
    }

    if (dir == tuya_codec::DIR_REQUEST) {
        // If a previous request is still pending, flush it as missed.
        if (s_have_pending) {
            ESP_LOGW(TAG, "Missed response for prev req A=%u B=%u",
                     s_pending_field_a, s_pending_field_b);
            s_pending.has_response = false;
            if (s_callback) s_callback(s_pending);
            s_txn_count.fetch_add(1, std::memory_order_relaxed);
        }
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.timestamp_ms = now_ms();
        s_pending.slave_addr   = 0;        // not used in Tuya
        s_pending.fc           = fc;
        s_pending.reg_addr     = win->reg_base;
        s_pending.reg_count    = (uint16_t)(b - win->prefix_len);  // arctic regs, not raw bytes
        s_pending.has_response = false;
        s_pending.error_code   = 0;
        s_pending_field_a   = a;
        s_pending_field_b   = b;
        s_pending_uptime_ms = esp_timer_get_time() / 1000;
        s_have_pending      = true;
        return;
    }

    // dir == DIR_RESPONSE
    if (!s_have_pending ||
        s_pending.fc        != fc ||
        s_pending_field_a   != a  ||
        s_pending_field_b   != b) {
        ESP_LOGD(TAG, "Orphan response A=%u B=%u (no matching pending)", a, b);
        s_parse_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Pull data bytes (skip prefix if any), unpack into Transaction.values[].
    const uint8_t *data = pf.payload + win->prefix_len;
    size_t data_bytes   = (size_t)pf.payload_len - win->prefix_len;
    if (data_bytes > MAX_REGS) data_bytes = MAX_REGS;

    Transaction txn = s_pending;
    txn.has_response = true;
    txn.timestamp_ms = now_ms();
    for (size_t i = 0; i < data_bytes; ++i) {
        uint16_t addr = (uint16_t)(win->reg_base + i);
        txn.values[i] = decode_byte(data[i], addr);
        snapshot_store(addr, data[i]);   // raw byte for OFF/ON diffing
        track_unknown(addr, data[i]);    // capture registers we don't know about
    }
    txn.reg_count = (uint16_t)data_bytes;

    s_have_pending = false;
    if (s_callback) s_callback(txn);
    s_txn_count.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Streaming frame extractor
// ---------------------------------------------------------------------------

static void consume_front(size_t n)
{
    if (n >= s_blob_len) {
        s_blob_len = 0;
    } else {
        memmove(s_blob_buf, s_blob_buf + n, s_blob_len - n);
        s_blob_len -= n;
    }
}

/// Capture raw bytes we're about to discard during resync into a ring buffer,
/// so /api/skipped can report what the stray inter-frame bytes actually are.
static void record_skipped(const uint8_t *buf, size_t n)
{
    int64_t ms = now_ms();
    for (size_t i = 0; i < n; ++i) {
        s_skipped_val[s_skipped_head] = buf[i];
        s_skipped_ms[s_skipped_head]  = ms;
        s_skipped_prevlen[s_skipped_head] = s_last_frame_len;
        s_skipped_prevdir[s_skipped_head] = s_last_frame_dir;
        s_skipped_prevfc[s_skipped_head]  = s_last_frame_fc;
        s_skipped_head = (s_skipped_head + 1) % SKIPPED_RING_SZ;
        s_skipped_total++;
    }
}

/// Find the next plausible frame start (`55 AA` followed by a sane header).
/// Returns offset, or s_blob_len if not found.
static size_t find_frame_start()
{
    return tuya_codec::find_frame_start(s_blob_buf, s_blob_len);
}

static void try_extract_frames()
{
    int iters = 0;
    while (s_blob_len >= tuya_codec::MIN_FRAME_LEN && iters++ < 32) {
        size_t start = find_frame_start();
        if (start == s_blob_len) {
            // No frame start found; drop everything except the last byte
            // (it might be the leading 0x55 of an incoming frame).
            if (s_blob_len > 1) {
                record_skipped(s_blob_buf, s_blob_len - 1);
                s_resync_bytes.fetch_add((uint32_t)(s_blob_len - 1),
                                         std::memory_order_relaxed);
                consume_front(s_blob_len - 1);
            }
            return;
        }
        if (start > 0) {
            // Pre-frame garbage / leftover bytes from an unrecognised frame.
            ESP_LOGD(TAG, "Skip %u bytes before next frame", (unsigned)start);
            record_skipped(s_blob_buf, start);
            s_resync_bytes.fetch_add((uint32_t)start, std::memory_order_relaxed);
            consume_front(start);
            continue;
        }

        // Valid header at offset 0. Compute frame length.
        uint8_t  dir = s_blob_buf[2];
        uint8_t  fc  = s_blob_buf[3];
        uint16_t bb  = (uint16_t)(s_blob_buf[6] << 8) | s_blob_buf[7];
        size_t flen = (fc == tuya_codec::FC_CMD)
                          ? (tuya_codec::HDR_LEN + tuya_codec::CHK_LEN)  // fixed 9
                          : tuya_codec::frame_total_len(dir, bb);
        if (flen == 0 || flen > tuya_codec::MAX_FRAME_LEN) {
            // Shouldn't happen given find_frame_start did header validation.
            ESP_LOGW(TAG, "Insane frame len %u for dir=0x%02X B=%u",
                     (unsigned)flen, dir, bb);
            s_parse_errors.fetch_add(1, std::memory_order_relaxed);
            consume_front(2);
            continue;
        }
        if (flen > s_blob_len) {
            // Need more bytes
            break;
        }

        log_hex("Frame", s_blob_buf, flen);
        s_frame_count.fetch_add(1, std::memory_order_relaxed);
        process_frame(s_blob_buf, flen);
        // Remember this frame's geometry so any bytes skipped before the NEXT
        // frame can be correlated against the frame that preceded them.
        s_last_frame_len = (uint16_t)flen;
        s_last_frame_dir = dir;
        s_last_frame_fc  = fc;
        consume_front(flen);
    }
}

// ---------------------------------------------------------------------------
// UART reader task — accumulates bytes and continuously extracts frames
// ---------------------------------------------------------------------------

static void sniffer_task(void *arg)
{
    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;
    uart_event_t event;
    constexpr int64_t RESP_TIMEOUT_MS = 500;
    int64_t last_stats_ms = 0;
    constexpr int64_t STATS_INTERVAL_MS = 10000;

    while (true) {
        bool got_event = xQueueReceive(s_uart_queue, &event, pdMS_TO_TICKS(10));

        if (got_event) {
            switch (event.type) {
                case UART_DATA: {
                    // Drain all available bytes from the ring buffer.
                    // Track which bytes are NEW in this event so we can
                    // emit a raw-byte record with natural burst boundaries.
                    const size_t blob_start = s_blob_len;
                    size_t avail = 0;
                    uart_get_buffered_data_len(port, &avail);
                    while (avail > 0 && s_blob_len < MAX_BLOB) {
                        size_t room = MAX_BLOB - s_blob_len;
                        size_t to_read = (avail < room) ? avail : room;
                        int got = uart_read_bytes(port,
                                                  s_blob_buf + s_blob_len,
                                                  to_read, pdMS_TO_TICKS(5));
                        if (got > 0) s_blob_len += got;
                        else break;
                        uart_get_buffered_data_len(port, &avail);
                    }
                    // If recording, emit this burst verbatim BEFORE the
                    // Modbus parser potentially consumes/discards bytes.
                    if (recorder::is_recording() && s_blob_len > blob_start) {
                        int64_t ts_ms = esp_timer_get_time() / 1000;
                        recorder::add_raw(s_blob_buf + blob_start,
                                          s_blob_len - blob_start,
                                          ts_ms);
                    }
                    break;
                }
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART buffer full — flushing");
                    uart_flush_input(port);
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(port);
                    xQueueReset(s_uart_queue);
                    s_blob_len = 0;
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGW(TAG, "UART parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGW(TAG, "UART frame error");
                    break;
                default:
                    break;
            }
        }

        // Continuously extract complete frames from the buffer
        try_extract_frames();

        // Response timeout for pending request
        if (s_have_pending) {
            int64_t uptime_ms = esp_timer_get_time() / 1000;
            if ((uptime_ms - s_pending_uptime_ms) > RESP_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Response timeout for FC 0x%02X addr %u",
                         s_pending.fc, s_pending.reg_addr);
                s_pending.has_response = false;
                if (s_callback) s_callback(s_pending);
                s_txn_count.fetch_add(1, std::memory_order_relaxed);
                s_have_pending = false;
            }
        }

        // Periodic stats summary
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - last_stats_ms) >= STATS_INTERVAL_MS) {
                ESP_LOGI(TAG, "Stats: frames=%lu txn=%lu parse_err=%lu resync_bytes=%lu buf=%u",
                         (unsigned long)s_frame_count.load(std::memory_order_relaxed),
                         (unsigned long)s_txn_count.load(std::memory_order_relaxed),
                         (unsigned long)s_parse_errors.load(std::memory_order_relaxed),
                         (unsigned long)s_resync_bytes.load(std::memory_order_relaxed),
                         (unsigned)s_blob_len);
                last_stats_ms = now_ms;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init(TransactionCallback cb)
{
    s_callback = cb;

    // Enable debug logging to diagnose frame parsing issues
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;

    // Configure UART. Boot defaults reflect what was actually observed on a
    // real Arctic ECO-600 + heat-pump pair (4800 baud, Odd parity gave the
    // cleanest decode in scope captures), not the V1.3 PDF (which claims
    // 2400 8E1). Both values are runtime-tunable via POST /api/config so we
    // can keep iterating without re-flashing.
    //
    // Note: We never transmit - just receive. The Atomic RS485 Base uses
    // auto-direction control, so as long as we don't call uart_write(),
    // we won't interfere with the bus.
    uart_config_t cfg = {};
    cfg.baud_rate  = CONFIG_SNIFFER_UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_ODD;   // Empirically cleanest on the real bus
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    // The Atomic RS485 base uses an auto-direction transceiver. We let the
    // UART peripheral claim the TX pin so it's actively driven idle-high,
    // which holds DE de-asserted and keeps the transceiver in RX. Leaving
    // TX as UART_PIN_NO_CHANGE causes DE to float and the driver to
    // intermittently enable, which jams the bus (controller logs E21).
    // We never call uart_write(), so nothing is ever actually transmitted.
    ESP_ERROR_CHECK(uart_set_pin(port,
                                 CONFIG_SNIFFER_UART_TX_PIN,
                                 CONFIG_SNIFFER_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Install UART driver WITH event queue
    ESP_ERROR_CHECK(uart_driver_install(port, UART_BUF_SZ * 2, 0,
                                        UART_QUEUE_SZ, &s_uart_queue, 0));

    // Hardware TOUT — fires after RX_TOUT_THRESH bit-times of bus silence
    // to deliver remaining FIFO bytes to the ring buffer.
    ESP_ERROR_CHECK(uart_set_rx_timeout(port, RX_TOUT_THRESH));

    ESP_LOGI(TAG, "UART%d init: %d baud, RX=GPIO%d, TX=GPIO%d (held idle), 8-O-1",
             port, CONFIG_SNIFFER_UART_BAUD, CONFIG_SNIFFER_UART_RX_PIN,
             CONFIG_SNIFFER_UART_TX_PIN);

    xTaskCreatePinnedToCore(sniffer_task, "sniffer", 8192, nullptr, 10, nullptr, 1);
    ESP_LOGI(TAG, "Sniffer task started");
}

uint32_t get_frame_count()       { return s_frame_count.load(std::memory_order_relaxed); }
uint32_t get_parse_errors()      { return s_parse_errors.load(std::memory_order_relaxed); }
uint32_t get_resync_bytes()      { return s_resync_bytes.load(std::memory_order_relaxed); }
uint32_t get_transaction_count() { return s_txn_count.load(std::memory_order_relaxed); }
uint32_t get_skipped_total()     { return s_skipped_total; }

size_t get_skipped_bytes(uint8_t *vals, int64_t *ms_out,
                         uint16_t *prevlen_out, uint8_t *prevdir_out,
                         uint8_t *prevfc_out, size_t max)
{
    if ((!vals && !ms_out && !prevlen_out && !prevdir_out && !prevfc_out) ||
        max == 0) {
        return 0;
    }
    uint32_t total = s_skipped_total;
    size_t   have  = (total < SKIPPED_RING_SZ) ? (size_t)total : SKIPPED_RING_SZ;
    if (have > max) have = max;
    // Emit oldest-first ending at the newest `have` entries.
    size_t start = (s_skipped_head + SKIPPED_RING_SZ - have) % SKIPPED_RING_SZ;
    for (size_t i = 0; i < have; ++i) {
        size_t idx = (start + i) % SKIPPED_RING_SZ;
        if (vals)        vals[i]        = s_skipped_val[idx];
        if (ms_out)      ms_out[i]      = s_skipped_ms[idx];
        if (prevlen_out) prevlen_out[i] = s_skipped_prevlen[idx];
        if (prevdir_out) prevdir_out[i] = s_skipped_prevdir[idx];
        if (prevfc_out)  prevfc_out[i]  = s_skipped_prevfc[idx];
    }
    return have;
}

static std::atomic<uint32_t> s_baud_rate{CONFIG_SNIFFER_UART_BAUD};
static std::atomic<bool> s_rx_inverted{false};
static std::atomic<int> s_parity{UART_PARITY_ODD};   // Empirically cleanest on the real bus
static std::atomic<int> s_stop_bits{UART_STOP_BITS_1};

uint32_t get_baud_rate() { return s_baud_rate.load(std::memory_order_relaxed); }

bool set_baud_rate(uint32_t baud)
{
    // Validate common baud rates
    if (baud != 1200 && baud != 2400 && baud != 4800 && baud != 9600 &&
        baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200) {
        ESP_LOGW(TAG, "Unsupported baud rate: %lu", (unsigned long)baud);
        return false;
    }

    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;
    
    // Flush and update baud rate
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));
    esp_err_t ret = uart_set_baudrate(port, baud);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set baud rate: %s", esp_err_to_name(ret));
        return false;
    }
    
    s_baud_rate.store(baud, std::memory_order_relaxed);
    
    // Clear buffer and reset stats to get clean readings at new rate
    uart_flush_input(port);
    s_blob_len = 0;
    s_have_pending = false;
    reset_counters();
    
    ESP_LOGI(TAG, "Baud rate changed to %lu", (unsigned long)baud);
    return true;
}

void reset_stats()
{
    reset_counters();
}

// ---------------------------------------------------------------------------
// Reverse-engineering aids: getters
// ---------------------------------------------------------------------------

uint32_t get_command_count()
{
    return s_cmd_count.load(std::memory_order_relaxed);
}

size_t get_recent_commands(CommandRec *out, size_t max)
{
    if (!out || max == 0) return 0;
    uint32_t total = s_cmd_count.load(std::memory_order_relaxed);
    size_t   have  = (total < COMMAND_RING_SZ) ? total : COMMAND_RING_SZ;
    if (have > max) have = max;
    // Emit oldest-first among the retained window.
    uint32_t start = total - (uint32_t)have;
    for (size_t i = 0; i < have; ++i) {
        out[i] = s_cmd_ring[(start + i) % COMMAND_RING_SZ];
    }
    return have;
}

size_t get_register_snapshot(RegisterSample *out, size_t max)
{
    if (!out || max == 0) return 0;
    size_t n = 0;
    for (uint16_t i = 0; i < SNAP_SPAN && n < max; ++i) {
        if (!s_snap_valid[i]) continue;
        out[n].addr = (uint16_t)(SNAP_BASE + i);
        out[n].raw  = s_snap_raw[i];
        ++n;
    }
    return n;
}

void clear_snapshot()
{
    memset(s_snap_raw, 0, sizeof(s_snap_raw));
    memset(s_snap_valid, 0, sizeof(s_snap_valid));
    s_cmd_count.store(0, std::memory_order_relaxed);
}

size_t get_unknown_registers(UnknownReg *out, size_t max)
{
    if (!out || max == 0) return 0;
    size_t n = 0;
    for (uint16_t i = 0; i < SNAP_SPAN && n < max; ++i) {
        if (!s_unk_valid[i]) continue;
        out[n].addr     = (uint16_t)(SNAP_BASE + i);
        out[n].last_raw = s_unk_raw[i];
        out[n].seen     = s_unk_seen[i];
        out[n].changes  = s_unk_changes[i];
        out[n].first_ms = s_unk_first[i];
        out[n].last_ms  = s_unk_last[i];
        ++n;
    }
    return n;
}

uint32_t get_unknown_count()
{
    return s_unk_count.load(std::memory_order_relaxed);
}

void clear_unknown_registers()
{
    memset(s_unk_raw, 0, sizeof(s_unk_raw));
    memset(s_unk_valid, 0, sizeof(s_unk_valid));
    memset(s_unk_seen, 0, sizeof(s_unk_seen));
    memset(s_unk_changes, 0, sizeof(s_unk_changes));
    memset(s_unk_first, 0, sizeof(s_unk_first));
    memset(s_unk_last, 0, sizeof(s_unk_last));
    s_unk_count.store(0, std::memory_order_relaxed);
}

bool get_rx_inverted() { return s_rx_inverted.load(std::memory_order_relaxed); }

bool set_rx_inverted(bool inverted)
{
    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;
    
    uart_flush_input(port);
    
    esp_err_t ret = uart_set_line_inverse(port, 
        inverted ? UART_SIGNAL_RXD_INV : UART_SIGNAL_INV_DISABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set RX inversion: %s", esp_err_to_name(ret));
        return false;
    }
    
    s_rx_inverted.store(inverted, std::memory_order_relaxed);
    
    // Clear buffer and reset stats
    s_blob_len = 0;
    s_have_pending = false;
    reset_counters();
    
    ESP_LOGI(TAG, "RX signal inversion: %s", inverted ? "ENABLED" : "DISABLED");
    return true;
}

Parity get_parity()
{
    int p = s_parity.load(std::memory_order_relaxed);
    if (p == UART_PARITY_EVEN) return Parity::EVEN;
    if (p == UART_PARITY_ODD) return Parity::ODD;
    return Parity::NONE;
}

bool set_parity(Parity p)
{
    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;
    
    uart_parity_t uart_p;
    const char *name;
    switch (p) {
        case Parity::EVEN: uart_p = UART_PARITY_EVEN; name = "EVEN"; break;
        case Parity::ODD:  uart_p = UART_PARITY_ODD;  name = "ODD";  break;
        default:           uart_p = UART_PARITY_DISABLE; name = "NONE"; break;
    }
    
    uart_flush_input(port);
    
    esp_err_t ret = uart_set_parity(port, uart_p);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set parity: %s", esp_err_to_name(ret));
        return false;
    }
    
    s_parity.store((int)uart_p, std::memory_order_relaxed);
    
    // Clear buffer and reset stats
    s_blob_len = 0;
    s_have_pending = false;
    reset_counters();
    
    ESP_LOGI(TAG, "Parity set to: %s", name);
    return true;
}

int get_stop_bits()
{
    return (s_stop_bits.load(std::memory_order_relaxed) == UART_STOP_BITS_2) ? 2 : 1;
}

bool set_stop_bits(int bits)
{
    if (bits != 1 && bits != 2) {
        ESP_LOGW(TAG, "Invalid stop bits: %d (must be 1 or 2)", bits);
        return false;
    }
    
    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;
    uart_stop_bits_t uart_sb = (bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    
    uart_flush_input(port);
    
    esp_err_t ret = uart_set_stop_bits(port, uart_sb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set stop bits: %s", esp_err_to_name(ret));
        return false;
    }
    
    s_stop_bits.store((int)uart_sb, std::memory_order_relaxed);
    
    // Clear buffer and reset stats
    s_blob_len = 0;
    s_have_pending = false;
    reset_counters();
    
    ESP_LOGI(TAG, "Stop bits set to: %d", bits);
    return true;
}

}  // namespace sniffer

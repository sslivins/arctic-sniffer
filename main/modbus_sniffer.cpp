#include "modbus_sniffer.h"
#include "arctic_registers.h"
#include "recorder.h"

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
// Tuya-framed Arctic protocol
//
// What's actually on the wire (NOT the V1.3 PDF's plain Modbus RTU):
//
//   55 AA <dir:1> <fc:1> <fieldA:2BE> <fieldB:2BE> [data:B bytes] <chk>
//
//   dir   : 0xF0 = controller -> heat pump (request)
//           0x0F = heat pump -> controller (response)
//   fc    : 0x03 = read register block (only FC observed)
//   fieldA: starting BYTE offset into the unified register page
//   fieldB: number of bytes to read
//   chk   : 1 byte on requests, 2 bytes on responses (algorithm unknown)
//
// Two polls are observed in rotation:
//   A=0  B=50 -> "telemetry" (input regs 2100+; bytes 0..6 are a static
//                7-byte prefix `0a 28 32 05 01 00 0f`, then byte 7 = reg
//                2100, byte 8 = reg 2101, etc.)
//   A=50 B=58 -> "holding"   (regs 2000..2057, byte 0 = reg 2000, no prefix)
//
// Registers are packed 1 byte each (NOT 2 bytes like classic Modbus).
// Apply per-register scale from arctic_registers.cpp; signed registers
// must be sign-extended from int8 -> int16 before storing in
// Transaction::values so to_signed() in arctic_registers.h decodes them
// correctly.
// ---------------------------------------------------------------------------

constexpr size_t  MAX_FRAME    = 256;
constexpr size_t  MAX_BLOB     = 512;   // max bytes in one UART batch
constexpr int     UART_BUF_SZ  = 1024;
constexpr int     UART_QUEUE_SZ = 20;
constexpr uint8_t RX_TOUT_THRESH = 40;  // hardware TOUT in bit-times

// Frame structure constants
constexpr uint8_t  TUYA_HDR0   = 0x55;
constexpr uint8_t  TUYA_HDR1   = 0xAA;
constexpr uint8_t  DIR_REQUEST  = 0xF0;
constexpr uint8_t  DIR_RESPONSE = 0x0F;
constexpr uint8_t  FC_READ      = 0x03;
constexpr size_t   TUYA_HDR_LEN = 8;   // 55 AA dir fc A:2 B:2
constexpr size_t   REQ_CHK_LEN  = 1;
constexpr size_t   RESP_CHK_LEN = 2;

// Known register windows (fieldA -> Arctic register base, prefix length)
struct RegWindow {
    uint16_t field_a;
    uint16_t field_b;     // expected B for this window
    uint16_t reg_base;    // first Arctic register number
    uint8_t  prefix_len;  // bytes at start of response data to skip
};
static constexpr RegWindow KNOWN_WINDOWS[] = {
    { 0,  50, 2100, 7 },  // telemetry (input regs)
    { 50, 58, 2000, 0 },  // holding regs
};

static const RegWindow *find_window(uint16_t a, uint16_t b)
{
    for (const auto &w : KNOWN_WINDOWS) {
        if (w.field_a == a && w.field_b == b) return &w;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static TransactionCallback s_callback;
static std::atomic<uint32_t> s_frame_count{0};
static std::atomic<uint32_t> s_crc_errors{0};   // now: framing/sanity rejects
static std::atomic<uint32_t> s_txn_count{0};

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
// Tuya frame helpers
// ---------------------------------------------------------------------------

/// Validate that `dir`, `fc`, and the (A,B) tuple match a known window.
/// Without a checksum to verify against, this is our only structural defense
/// against a corrupted `55 AA` byte pair starting a phantom frame.
static bool header_sane(uint8_t dir, uint8_t fc, uint16_t a, uint16_t b)
{
    if (dir != DIR_REQUEST && dir != DIR_RESPONSE) return false;
    if (fc  != FC_READ) return false;
    return find_window(a, b) != nullptr;
}

/// Compute the total frame length (header + data + chk) given a peeked
/// header. Returns 0 if more bytes are needed before we can decide,
/// or a value > MAX_FRAME if the header is unreasonable.
static size_t tuya_frame_len(uint8_t dir, uint16_t b)
{
    if (dir == DIR_REQUEST)  return TUYA_HDR_LEN + REQ_CHK_LEN;            // 9
    if (dir == DIR_RESPONSE) return TUYA_HDR_LEN + b + RESP_CHK_LEN;       // 10+B
    return 0;
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
    // buf[0..1] = 55 AA (already verified)
    uint8_t  dir = buf[2];
    uint8_t  fc  = buf[3];
    uint16_t a   = (uint16_t)(buf[4] << 8) | buf[5];
    uint16_t b   = (uint16_t)(buf[6] << 8) | buf[7];

    const RegWindow *win = find_window(a, b);
    if (!win) return;  // header_sane should have caught this

    if (dir == DIR_REQUEST) {
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
        s_crc_errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Pull data bytes (skip prefix if any), unpack into Transaction.values[].
    const uint8_t *data = buf + TUYA_HDR_LEN + win->prefix_len;
    size_t data_bytes   = (size_t)b - win->prefix_len;
    if (data_bytes > MAX_REGS) data_bytes = MAX_REGS;

    Transaction txn = s_pending;
    txn.has_response = true;
    txn.timestamp_ms = now_ms();
    for (size_t i = 0; i < data_bytes; ++i) {
        uint16_t addr = (uint16_t)(win->reg_base + i);
        txn.values[i] = decode_byte(data[i], addr);
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

/// Find the next plausible frame start (`55 AA` followed by a sane header).
/// Returns offset, or s_blob_len if not found.
static size_t find_frame_start()
{
    // Need at least 8 bytes (header) to validate sanity
    if (s_blob_len < TUYA_HDR_LEN) return s_blob_len;
    for (size_t i = 0; i + TUYA_HDR_LEN <= s_blob_len; ++i) {
        if (s_blob_buf[i] != TUYA_HDR0) continue;
        if (s_blob_buf[i + 1] != TUYA_HDR1) continue;
        uint8_t  dir = s_blob_buf[i + 2];
        uint8_t  fc  = s_blob_buf[i + 3];
        uint16_t a   = (uint16_t)(s_blob_buf[i + 4] << 8) | s_blob_buf[i + 5];
        uint16_t bb  = (uint16_t)(s_blob_buf[i + 6] << 8) | s_blob_buf[i + 7];
        if (header_sane(dir, fc, a, bb)) return i;
    }
    return s_blob_len;
}

static void try_extract_frames()
{
    int iters = 0;
    while (s_blob_len >= TUYA_HDR_LEN + REQ_CHK_LEN && iters++ < 32) {
        size_t start = find_frame_start();
        if (start == s_blob_len) {
            // No frame start found; drop everything except the last byte
            // (it might be the leading 0x55 of an incoming frame).
            if (s_blob_len > 1) {
                s_crc_errors.fetch_add((uint32_t)(s_blob_len - 1),
                                       std::memory_order_relaxed);
                consume_front(s_blob_len - 1);
            }
            return;
        }
        if (start > 0) {
            // Pre-frame garbage / leftover bytes from an unrecognised frame.
            ESP_LOGD(TAG, "Skip %u bytes before next frame", (unsigned)start);
            s_crc_errors.fetch_add((uint32_t)start, std::memory_order_relaxed);
            consume_front(start);
            continue;
        }

        // Valid header at offset 0. Compute frame length.
        uint8_t  dir = s_blob_buf[2];
        uint16_t bb  = (uint16_t)(s_blob_buf[6] << 8) | s_blob_buf[7];
        size_t flen = tuya_frame_len(dir, bb);
        if (flen == 0 || flen > MAX_FRAME) {
            // Shouldn't happen given header_sane, but be defensive.
            ESP_LOGW(TAG, "Insane frame len %u for dir=0x%02X B=%u",
                     (unsigned)flen, dir, bb);
            s_crc_errors.fetch_add(1, std::memory_order_relaxed);
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
                ESP_LOGI(TAG, "Stats: frames=%lu txn=%lu crc_err=%lu buf=%u",
                         (unsigned long)s_frame_count.load(std::memory_order_relaxed),
                         (unsigned long)s_txn_count.load(std::memory_order_relaxed),
                         (unsigned long)s_crc_errors.load(std::memory_order_relaxed),
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
uint32_t get_crc_errors()        { return s_crc_errors.load(std::memory_order_relaxed); }
uint32_t get_transaction_count() { return s_txn_count.load(std::memory_order_relaxed); }

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
    s_frame_count.store(0, std::memory_order_relaxed);
    s_crc_errors.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
    
    ESP_LOGI(TAG, "Baud rate changed to %lu", (unsigned long)baud);
    return true;
}

void reset_stats()
{
    s_frame_count.store(0, std::memory_order_relaxed);
    s_crc_errors.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
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
    s_frame_count.store(0, std::memory_order_relaxed);
    s_crc_errors.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
    
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
    s_frame_count.store(0, std::memory_order_relaxed);
    s_crc_errors.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
    
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
    s_frame_count.store(0, std::memory_order_relaxed);
    s_crc_errors.store(0, std::memory_order_relaxed);
    s_txn_count.store(0, std::memory_order_relaxed);
    
    ESP_LOGI(TAG, "Stop bits set to: %d", bits);
    return true;
}

}  // namespace sniffer

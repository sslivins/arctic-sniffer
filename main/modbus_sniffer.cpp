#include "modbus_sniffer.h"
#include "arctic_registers.h"

#include <cstring>
#include <atomic>
#include <sys/time.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sniffer";

namespace sniffer {

// ---------------------------------------------------------------------------
// Modbus CRC-16 (polynomial 0xA001)
// ---------------------------------------------------------------------------

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Frame buffer
// ---------------------------------------------------------------------------

constexpr size_t  MAX_FRAME    = 256;
constexpr size_t  MAX_BLOB     = 512;   // max bytes in one UART batch
constexpr int     UART_BUF_SZ  = 1024;
constexpr int     UART_QUEUE_SZ = 20;
constexpr uint8_t RX_TOUT_THRESH = 40;  // hardware TOUT in bit-times

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static TransactionCallback s_callback;
static std::atomic<uint32_t> s_frame_count{0};
static std::atomic<uint32_t> s_crc_errors{0};
static std::atomic<uint32_t> s_txn_count{0};

// UART event queue — filled by the driver ISR
static QueueHandle_t s_uart_queue = nullptr;

// Accumulation buffer — collects bytes until TOUT, then split into frames
static uint8_t  s_blob_buf[MAX_BLOB];
static size_t   s_blob_len = 0;

// Pending request (waiting for matching response)
static bool     s_have_pending = false;
static Transaction s_pending;
static int64_t  s_pending_uptime_ms = 0;  // uptime when request arrived (for timeout)

// ---------------------------------------------------------------------------
// Frame parser
// ---------------------------------------------------------------------------

/// Check CRC for a candidate frame of given length.
/// Returns true if valid.
static bool crc_ok(const uint8_t *buf, size_t len)
{
    if (len < 4) return false;
    uint16_t received = buf[len - 2] | (buf[len - 1] << 8);
    uint16_t calc     = crc16(buf, len - 2);
    return received == calc;
}

/// Return true if the function code is one we understand.
static bool is_known_fc(uint8_t fc)
{
    return fc == 0x03 || fc == 0x04 || fc == 0x06 || fc == 0x10 ||
           (fc & 0x80);
}

/// Given a byte at offset 1 (the function code), return the expected PDU
/// length INCLUDING addr + FC + CRC, or 0 if we can't determine it yet
/// (need more bytes).  Uses the pairing state (s_have_pending) to decide
/// whether the frame is most likely a request or a response when the FC
/// is ambiguous (0x03/0x04).
static size_t expected_frame_len(const uint8_t *buf, size_t remaining)
{
    if (remaining < 2) return 0;
    uint8_t fc = buf[1];

    switch (fc) {
        case 0x03:  // Read Holding Registers
        case 0x04:  // Read Input Registers
            // REQUEST:  addr(1) + FC(1) + start(2) + count(2) + CRC(2) = 8
            // RESPONSE: addr(1) + FC(1) + byte_count(1) + data(N) + CRC(2)
            if (s_have_pending) {
                // Expect response first
                if (remaining >= 3) {
                    size_t resp_len = 3 + buf[2] + 2;
                    if (resp_len >= 5 && resp_len <= remaining) return resp_len;
                }
                // Maybe it's actually a new request (missed response)
                if (remaining >= 8) return 8;
            } else {
                // Expect request first
                if (remaining >= 8) return 8;
                // Maybe it's a response we joined mid-stream
                if (remaining >= 3) {
                    size_t resp_len = 3 + buf[2] + 2;
                    if (resp_len >= 5 && resp_len <= remaining) return resp_len;
                }
            }
            return 0;

        case 0x06:  // Write Single Register
            // Both request and response are 8 bytes
            return (remaining >= 8) ? 8 : 0;

        case 0x10:  // Write Multiple Registers
            if (s_have_pending) {
                // Expect response (echo): 8 bytes
                if (remaining >= 8) return 8;
            } else {
                // Expect request: addr(1)+FC(1)+start(2)+count(2)+bc(1)+data(N)+CRC(2)
                if (remaining >= 7) {
                    size_t req_len = 7 + buf[6] + 2;
                    if (req_len <= remaining) return req_len;
                }
                return 0;
            }
            return 0;

        default:
            // Error response: FC | 0x80 → 5 bytes
            if ((fc & 0x80) && remaining >= 5) return 5;
            return 0;
    }
}

/// Process a validated single frame in the request/response state machine.
static void process_frame(const uint8_t *buf, size_t len)
{
    if (len < 4 || !crc_ok(buf, len)) return;

    uint8_t addr = buf[0];
    uint8_t fc   = buf[1];
    const uint8_t *payload = buf + 2;
    size_t plen = len - 4;  // minus addr + fc + 2 CRC bytes

    // Use wall-clock time if NTP has synced, otherwise fall back to uptime
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t now_ms;
    if (tv.tv_sec > 1577836800) {  // after 2020-01-01 → NTP synced
        now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    } else {
        now_ms = esp_timer_get_time() / 1000;
    }

    // Is this a request (from master) or response (from slave)?
    // Convention: on a bus with one master, the master sends requests.
    // Requests use FC 0x03/0x06/0x10.  Responses echo the FC (or FC|0x80 for errors).
    // We distinguish by pairing: if we don't have a pending request, this is a request.

    if (!s_have_pending) {
        // Treat as a request
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.timestamp_ms = now_ms;
        s_pending_uptime_ms = esp_timer_get_time() / 1000;
        s_pending.slave_addr = addr;
        s_pending.fc = fc;
        s_pending.has_response = false;
        s_pending.error_code = 0;

        switch (fc) {
            case 0x03: {
                // Read Holding Registers: addr(2) + count(2)
                if (plen >= 4) {
                    s_pending.reg_addr  = (payload[0] << 8) | payload[1];
                    s_pending.reg_count = (payload[2] << 8) | payload[3];
                }
                s_have_pending = true;
                break;
            }
            case 0x06: {
                // Write Single Register: addr(2) + value(2)
                if (plen >= 4) {
                    s_pending.reg_addr  = (payload[0] << 8) | payload[1];
                    s_pending.reg_count = 1;
                    s_pending.values[0] = (payload[2] << 8) | payload[3];
                }
                s_have_pending = true;
                break;
            }
            case 0x10: {
                // Write Multiple Registers: addr(2) + count(2) + byte_count(1) + data
                if (plen >= 5) {
                    s_pending.reg_addr  = (payload[0] << 8) | payload[1];
                    s_pending.reg_count = (payload[2] << 8) | payload[3];
                    uint8_t byte_count  = payload[4];
                    size_t n = s_pending.reg_count;
                    if (n > MAX_REGS) n = MAX_REGS;
                    for (size_t i = 0; i < n && (5 + i * 2 + 1) < plen; ++i) {
                        s_pending.values[i] = (payload[5 + i * 2] << 8) | payload[5 + i * 2 + 1];
                    }
                    (void)byte_count;
                }
                s_have_pending = true;
                break;
            }
            default:
                // Unknown FC — emit as standalone
                s_pending.has_response = false;
                if (s_callback) s_callback(s_pending);
                s_txn_count.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    } else {
        // Treat as a response to the pending request
        Transaction txn = s_pending;
        txn.has_response = true;
        s_have_pending = false;

        // Error response?
        if (fc & 0x80) {
            txn.error_code = (plen >= 1) ? payload[0] : 0xFF;
        } else {
            switch (fc) {
                case 0x03: {
                    // Read response: byte_count(1) + data
                    if (plen >= 1) {
                        uint8_t byte_count = payload[0];
                        size_t n = byte_count / 2;
                        if (n > MAX_REGS) n = MAX_REGS;
                        if (n > txn.reg_count) n = txn.reg_count;
                        for (size_t i = 0; i < n && (1 + i * 2 + 1) < plen; ++i) {
                            txn.values[i] = (payload[1 + i * 2] << 8) | payload[1 + i * 2 + 1];
                        }
                        txn.reg_count = n;
                    }
                    break;
                }
                case 0x06: {
                    // Echo: addr(2) + value(2)
                    if (plen >= 4) {
                        txn.values[0] = (payload[2] << 8) | payload[3];
                    }
                    break;
                }
                case 0x10: {
                    // Echo: addr(2) + count(2)
                    // Values already captured from the request
                    break;
                }
                default:
                    break;
            }
        }

        if (s_callback) s_callback(txn);
        s_txn_count.fetch_add(1, std::memory_order_relaxed);
    }
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
// Streaming frame extractor — pulls complete frames from the front of the
// accumulation buffer as soon as they are available, regardless of UART
// chunk boundaries.
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

static void try_extract_frames()
{
    // Limit iterations to prevent pathological loops
    int iters = 0;
    while (s_blob_len >= 4 && iters++ < 32) {
        // --- 1. Try structure-based extraction ---
        size_t flen = expected_frame_len(s_blob_buf, s_blob_len);

        if (flen > 0 && flen <= s_blob_len) {
            if (crc_ok(s_blob_buf, flen)) {
                log_hex("Frame", s_blob_buf, flen);
                s_frame_count.fetch_add(1, std::memory_order_relaxed);
                process_frame(s_blob_buf, flen);
                consume_front(flen);
                continue;
            }
            // CRC failed at expected length — structure guess was wrong.
            // Fall through to brute-force.
        } else if (flen > s_blob_len) {
            // Frame not fully received yet — wait for more bytes
            break;
        }
        // flen == 0: either unknown FC or not enough bytes for length peek.

        // --- 2. Unknown FC → skip byte (garbage) ---
        if (!is_known_fc(s_blob_buf[1])) {
            ESP_LOGD(TAG, "Skip unknown FC 0x%02X at buf[0]=0x%02X",
                     s_blob_buf[1], s_blob_buf[0]);
            s_crc_errors.fetch_add(1, std::memory_order_relaxed);
            consume_front(1);
            continue;
        }

        // --- 3. Known FC but structure check failed — brute-force CRC ---
        bool found = false;
        size_t max_try = (s_blob_len < MAX_FRAME) ? s_blob_len : MAX_FRAME;
        for (size_t try_len = 4; try_len <= max_try; ++try_len) {
            if (crc_ok(s_blob_buf, try_len)) {
                ESP_LOGD(TAG, "Brute-force frame len=%u", (unsigned)try_len);
                log_hex("Frame(bf)", s_blob_buf, try_len);
                s_frame_count.fetch_add(1, std::memory_order_relaxed);
                process_frame(s_blob_buf, try_len);
                consume_front(try_len);
                found = true;
                break;
            }
        }
        if (found) continue;

        // --- 4. No valid frame yet ---
        if (s_blob_len > MAX_FRAME) {
            // Buffer is huge and nothing matched — skip a byte
            s_crc_errors.fetch_add(1, std::memory_order_relaxed);
            consume_front(1);
        } else {
            // Might just need more bytes — wait
            break;
        }
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
                    // Drain all available bytes from the ring buffer
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

    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;

    uart_config_t cfg = {};
    cfg.baud_rate  = CONFIG_SNIFFER_UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_EVEN;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
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

    ESP_LOGI(TAG, "UART%d init: %d baud, RX=GPIO%d, 8-E-1",
             port, CONFIG_SNIFFER_UART_BAUD, CONFIG_SNIFFER_UART_RX_PIN);

    xTaskCreatePinnedToCore(sniffer_task, "sniffer", 8192, nullptr, 10, nullptr, 1);
    ESP_LOGI(TAG, "Sniffer task started");
}

uint32_t get_frame_count()       { return s_frame_count.load(std::memory_order_relaxed); }
uint32_t get_crc_errors()        { return s_crc_errors.load(std::memory_order_relaxed); }
uint32_t get_transaction_count() { return s_txn_count.load(std::memory_order_relaxed); }

}  // namespace sniffer

#include "modbus_sniffer.h"
#include "arctic_registers.h"

#include <cstring>
#include <atomic>

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
constexpr int     UART_BUF_SZ  = 512;

// Inter-frame gap.  At 2400 baud, 8-E-1 = 11 bits/char → ~4.58 ms/char.
// 3.5 char times ≈ 16 ms.  Use a slightly generous value.
constexpr int64_t FRAME_GAP_US = 16000;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static TransactionCallback s_callback;
static std::atomic<uint32_t> s_frame_count{0};
static std::atomic<uint32_t> s_crc_errors{0};
static std::atomic<uint32_t> s_txn_count{0};

// Current frame being assembled
static uint8_t  s_frame_buf[MAX_FRAME];
static size_t   s_frame_len = 0;
static int64_t  s_last_byte_time = 0;  // microseconds

// Pending request (waiting for matching response)
static bool     s_have_pending = false;
static Transaction s_pending;

// ---------------------------------------------------------------------------
// Frame parser
// ---------------------------------------------------------------------------

/// Parse a complete Modbus RTU frame.
/// Returns true if valid (correct CRC, minimum length).
static bool parse_frame(const uint8_t *buf, size_t len,
                         uint8_t &addr, uint8_t &fc,
                         const uint8_t *&payload, size_t &payload_len)
{
    if (len < 4) return false;  // addr + fc + crc16 minimum

    // CRC check
    uint16_t received_crc = buf[len - 2] | (buf[len - 1] << 8);
    uint16_t calc_crc = crc16(buf, len - 2);
    if (received_crc != calc_crc) {
        s_crc_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    addr = buf[0];
    fc   = buf[1];
    payload = buf + 2;
    payload_len = len - 4;  // minus addr + fc + 2 CRC bytes
    return true;
}

/// Process a validated frame in context of the request/response state machine.
static void process_frame(const uint8_t *buf, size_t len)
{
    uint8_t addr, fc;
    const uint8_t *payload;
    size_t plen;

    if (!parse_frame(buf, len, addr, fc, payload, plen))
        return;

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Is this a request (from master) or response (from slave)?
    // Convention: on a bus with one master, the master sends requests.
    // Requests use FC 0x03/0x06/0x10.  Responses echo the FC (or FC|0x80 for errors).
    // We distinguish by pairing: if we don't have a pending request, this is a request.

    if (!s_have_pending) {
        // Treat as a request
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.timestamp_ms = now_ms;
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
// UART receive task
// ---------------------------------------------------------------------------

static void sniffer_task(void *arg)
{
    uint8_t rx_buf[UART_BUF_SZ];
    const uart_port_t port = (uart_port_t)CONFIG_SNIFFER_UART_PORT;

    while (true) {
        int len = uart_read_bytes(port, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(5));
        int64_t now_us = esp_timer_get_time();

        if (len > 0) {
            for (int i = 0; i < len; ++i) {
                // Detect inter-frame gap
                if (s_frame_len > 0 && (now_us - s_last_byte_time) > FRAME_GAP_US) {
                    // Gap detected — process completed frame
                    s_frame_count.fetch_add(1, std::memory_order_relaxed);
                    process_frame(s_frame_buf, s_frame_len);
                    s_frame_len = 0;
                }

                // Append byte
                if (s_frame_len < MAX_FRAME) {
                    s_frame_buf[s_frame_len++] = rx_buf[i];
                }
                s_last_byte_time = now_us;
            }
        } else {
            // No data received — check if pending frame should be flushed
            if (s_frame_len > 0 && (now_us - s_last_byte_time) > FRAME_GAP_US) {
                s_frame_count.fetch_add(1, std::memory_order_relaxed);
                process_frame(s_frame_buf, s_frame_len);
                s_frame_len = 0;
            }

            // If we have a pending request with no response for > 500 ms, emit it solo
            if (s_have_pending) {
                int64_t now_ms = now_us / 1000;
                if ((now_ms - s_pending.timestamp_ms) > 500) {
                    ESP_LOGW(TAG, "Response timeout for FC 0x%02X addr %u",
                             s_pending.fc, s_pending.reg_addr);
                    s_pending.has_response = false;
                    if (s_callback) s_callback(s_pending);
                    s_txn_count.fetch_add(1, std::memory_order_relaxed);
                    s_have_pending = false;
                }
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
    ESP_ERROR_CHECK(uart_driver_install(port, UART_BUF_SZ * 2, 0, 0, nullptr, 0));

    ESP_LOGI(TAG, "UART%d init: %d baud, RX=GPIO%d, 8-E-1",
             port, CONFIG_SNIFFER_UART_BAUD, CONFIG_SNIFFER_UART_RX_PIN);

    xTaskCreatePinnedToCore(sniffer_task, "sniffer", 4096, nullptr, 10, nullptr, 1);
    ESP_LOGI(TAG, "Sniffer task started");
}

uint32_t get_frame_count()       { return s_frame_count.load(std::memory_order_relaxed); }
uint32_t get_crc_errors()        { return s_crc_errors.load(std::memory_order_relaxed); }
uint32_t get_transaction_count() { return s_txn_count.load(std::memory_order_relaxed); }

}  // namespace sniffer

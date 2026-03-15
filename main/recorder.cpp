#include "recorder.h"
#include "arctic_registers.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <functional>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

static const char *TAG = "recorder";

namespace recorder {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// Target buffer size when PSRAM is available (4 MB).
static constexpr size_t PSRAM_BUFFER_SIZE = 4 * 1024 * 1024;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static std::mutex  s_mutex;
static bool        s_recording   = false;
static char       *s_buffer      = nullptr;   // raw buffer (PSRAM)
static size_t      s_buffer_cap  = 0;         // allocated capacity
static size_t      s_buffer_used = 0;         // bytes written
static size_t      s_entry_count = 0;
static std::function<void()> s_auto_stop_cb;

// ---------------------------------------------------------------------------
// JSONL formatting (shared by in-memory recording & web streaming)
// ---------------------------------------------------------------------------

/// Format epoch ms manually — %lld is unreliable on Xtensa toolchain
static void format_i64(int64_t v, char *out)
{
    bool neg = v < 0;
    if (neg) v = -v;
    char tmp[24];
    char *p = tmp + sizeof(tmp) - 1;
    *p = '\0';
    do { *--p = '0' + (v % 10); v /= 10; } while (v);
    if (neg) *--p = '-';
    strcpy(out, p);
}

int format_jsonl(const sniffer::Transaction &txn, char *buf, size_t buf_len)
{
    char ts_buf[24];
    format_i64(txn.timestamp_ms, ts_buf);

    int off = 0;

    switch (txn.fc) {
        case 0x03: {
            off = snprintf(buf, buf_len,
                           "{\"t\":%s,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                           ts_buf, txn.reg_addr, txn.reg_count);
            for (uint16_t i = 0; i < txn.reg_count && off < (int)buf_len - 16; ++i) {
                if (i > 0) buf[off++] = ',';
                off += snprintf(buf + off, buf_len - off, "%u", txn.values[i]);
            }
            off += snprintf(buf + off, buf_len - off, "]}\n");
            break;
        }
        case 0x06: {
            off = snprintf(buf, buf_len,
                           "{\"t\":%s,\"fc\":6,\"addr\":%u,\"value\":%u}\n",
                           ts_buf, txn.reg_addr, txn.values[0]);
            break;
        }
        case 0x10: {
            off = snprintf(buf, buf_len,
                           "{\"t\":%s,\"fc\":16,\"addr\":%u,\"count\":%u,\"values\":[",
                           ts_buf, txn.reg_addr, txn.reg_count);
            for (uint16_t i = 0; i < txn.reg_count && off < (int)buf_len - 16; ++i) {
                if (i > 0) buf[off++] = ',';
                off += snprintf(buf + off, buf_len - off, "%u", txn.values[i]);
            }
            off += snprintf(buf + off, buf_len - off, "]}\n");
            break;
        }
        default:
            return 0;
    }
    return off;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init()
{
    if (esp_psram_is_initialized()) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t alloc_size = PSRAM_BUFFER_SIZE;
        if (alloc_size > free_psram - (64 * 1024)) {
            // Leave 64 KB headroom in PSRAM
            alloc_size = free_psram > (64 * 1024) ? free_psram - (64 * 1024) : 0;
        }

        if (alloc_size > 0) {
            s_buffer = (char *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
            if (s_buffer) {
                s_buffer_cap = alloc_size;
                ESP_LOGI(TAG, "Recording buffer: %uKB in PSRAM",
                         (unsigned)(alloc_size / 1024));
            } else {
                ESP_LOGW(TAG, "PSRAM alloc failed — in-memory recording disabled");
            }
        }
    } else {
        ESP_LOGI(TAG, "No PSRAM — in-memory recording disabled (web streaming OK)");
    }
}

bool has_memory_recording()
{
    return s_buffer != nullptr;
}

void start()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_buffer) {
        ESP_LOGW(TAG, "Cannot start — no recording buffer");
        return;
    }
    s_buffer_used = 0;
    s_entry_count = 0;
    s_recording = true;
    ESP_LOGI(TAG, "Recording started (buffer: %uKB)",
             (unsigned)(s_buffer_cap / 1024));
}

void stop()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_recording = false;
    ESP_LOGI(TAG, "Recording stopped — %u entries, %uKB",
             (unsigned)s_entry_count, (unsigned)(s_buffer_used / 1024));
}

bool is_recording()
{
    return s_recording;
}

void add(const sniffer::Transaction &txn)
{
    std::unique_lock<std::mutex> lock(s_mutex);
    if (!s_recording || !s_buffer) return;

    size_t remaining = s_buffer_cap - s_buffer_used;
    if (remaining < 32) {
        // Not enough space — auto-stop
        s_recording = false;
        ESP_LOGW(TAG, "Recording auto-stopped — buffer full (%uKB)",
                 (unsigned)(s_buffer_used / 1024));
        if (s_auto_stop_cb) {
            lock.unlock();
            s_auto_stop_cb();
        }
        return;
    }

    int written = format_jsonl(txn, s_buffer + s_buffer_used, remaining);
    if (written > 0) {
        s_buffer_used += written;
        s_entry_count++;
    }

    // Check if nearly full after write
    if (s_buffer_used >= s_buffer_cap - 32) {
        s_recording = false;
        ESP_LOGW(TAG, "Recording auto-stopped — buffer full (%uKB)",
                 (unsigned)(s_buffer_used / 1024));
        if (s_auto_stop_cb) {
            lock.unlock();
            s_auto_stop_cb();
        }
    }
}

const char *get_data(size_t &len)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    len = s_buffer_used;
    return s_buffer;
}

size_t get_entry_count()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_entry_count;
}

void clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_buffer_used = 0;
    s_entry_count = 0;
    ESP_LOGI(TAG, "Recording data cleared");
}

void set_auto_stop_callback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_auto_stop_cb = cb;
}

size_t get_buffer_used()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_buffer_used;
}

size_t get_buffer_limit()
{
    return s_buffer_cap;
}

}  // namespace recorder

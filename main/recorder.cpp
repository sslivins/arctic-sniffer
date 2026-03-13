#include "recorder.h"
#include "arctic_registers.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <functional>

#include "esp_log.h"

static const char *TAG = "recorder";

namespace recorder {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static std::mutex  s_mutex;
static bool        s_recording = false;
static std::string s_buffer;          // JSONL accumulator
static size_t      s_entry_count = 0;
static std::function<void()> s_auto_stop_cb;

// Reserve 128 KB initial buffer to reduce reallocations
constexpr size_t INITIAL_RESERVE = 128 * 1024;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void start()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_buffer.clear();
    s_buffer.reserve(INITIAL_RESERVE);
    s_entry_count = 0;
    s_recording = true;
    ESP_LOGI(TAG, "Recording started");
}

void stop()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_recording = false;
    ESP_LOGI(TAG, "Recording stopped — %u entries, %u bytes",
             (unsigned)s_entry_count, (unsigned)s_buffer.size());
}

bool is_recording()
{
    return s_recording;
}

void add(const sniffer::Transaction &txn)
{
    std::unique_lock<std::mutex> lock(s_mutex);
    if (!s_recording) return;

    // Format epoch ms manually — %lld unreliable on Xtensa
    char ts_buf[24];
    {
        int64_t v = txn.timestamp_ms;
        bool neg = v < 0;
        if (neg) v = -v;
        char *p = ts_buf + sizeof(ts_buf) - 1;
        *p = '\0';
        do { *--p = '0' + (v % 10); v /= 10; } while (v);
        if (neg) *--p = '-';
        memmove(ts_buf, p, ts_buf + sizeof(ts_buf) - p);
    }

    char line[1024];
    int off = 0;

    switch (txn.fc) {
        case 0x03: {
            // Read Holding — emit as fc=3 with values from response
            off = snprintf(line, sizeof(line),
                           "{\"t\":%s,\"fc\":3,\"addr\":%u,\"count\":%u,\"values\":[",
                           ts_buf, txn.reg_addr, txn.reg_count);
            for (uint16_t i = 0; i < txn.reg_count && off < (int)sizeof(line) - 16; ++i) {
                if (i > 0) line[off++] = ',';
                off += snprintf(line + off, sizeof(line) - off, "%u", txn.values[i]);
            }
            off += snprintf(line + off, sizeof(line) - off, "]}\n");
            break;
        }
        case 0x06: {
            // Write Single
            off = snprintf(line, sizeof(line),
                           "{\"t\":%s,\"fc\":6,\"addr\":%u,\"value\":%u}\n",
                           ts_buf, txn.reg_addr, txn.values[0]);
            break;
        }
        case 0x10: {
            // Write Multiple
            off = snprintf(line, sizeof(line),
                           "{\"t\":%s,\"fc\":16,\"addr\":%u,\"count\":%u,\"values\":[",
                           ts_buf, txn.reg_addr, txn.reg_count);
            for (uint16_t i = 0; i < txn.reg_count && off < (int)sizeof(line) - 16; ++i) {
                if (i > 0) line[off++] = ',';
                off += snprintf(line + off, sizeof(line) - off, "%u", txn.values[i]);
            }
            off += snprintf(line + off, sizeof(line) - off, "]}\n");
            break;
        }
        default:
            return;  // Skip unknown function codes
    }

    s_buffer.append(line, off);
    s_entry_count++;

    // Auto-stop if buffer is full
    if (s_buffer.size() >= MAX_BUFFER_SIZE) {
        s_recording = false;
        ESP_LOGW(TAG, "Recording auto-stopped — buffer full (%u bytes)",
                 (unsigned)s_buffer.size());
        if (s_auto_stop_cb) {
            // Release lock before callback to avoid deadlock
            lock.unlock();
            s_auto_stop_cb();
        }
    }
}

const char *get_data(size_t &len)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    len = s_buffer.size();
    return s_buffer.c_str();
}

size_t get_entry_count()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_entry_count;
}

void clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_buffer.clear();
    s_buffer.shrink_to_fit();
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
    return s_buffer.size();
}

size_t get_buffer_limit()
{
    return MAX_BUFFER_SIZE;
}

}  // namespace recorder

#pragma once

#include "modbus_sniffer.h"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace recorder {

/// Maximum recording buffer size (bytes).  Recording auto-stops when full.
constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256 KB

/// Start recording.  Clears any existing buffer.
void start();

/// Stop recording.
void stop();

/// Returns true if currently recording.
bool is_recording();

/// Set a callback invoked when recording auto-stops (buffer full).
/// Called from the sniffer task context.
void set_auto_stop_callback(std::function<void()> cb);

/// Add a transaction to the recording buffer (called from sniffer callback).
/// Thread-safe.  Auto-stops recording if the buffer is full.
void add(const sniffer::Transaction &txn);

/// Get the recorded JSONL data.  Returns a pointer to the internal buffer
/// and sets `len` to the number of bytes.  Caller must NOT free.
/// Only valid while recording is stopped.
const char *get_data(size_t &len);

/// Get the number of recorded entries.
size_t get_entry_count();

/// Get current buffer usage in bytes.
size_t get_buffer_used();

/// Get maximum buffer capacity in bytes.
size_t get_buffer_limit();

/// Clear all recorded data and free memory.
void clear();

}  // namespace recorder

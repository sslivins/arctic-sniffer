#pragma once

#include "modbus_sniffer.h"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace recorder {

/// Initialise the recorder.  If PSRAM is available, allocates a large
/// buffer from PSRAM for in-memory recording (button-triggered on S3R).
/// Without PSRAM, in-memory recording is disabled but web-based
/// streaming still works.
void init();

/// Returns true if the device has PSRAM and in-memory recording is
/// available (Atom S3R).
bool has_memory_recording();

/// Start in-memory recording.  Only works if has_memory_recording().
/// Clears any existing buffer.
void start();

/// Stop recording.
void stop();

/// Returns true if currently recording to memory.
bool is_recording();

/// Set a callback invoked when recording auto-stops (buffer full).
/// Called from the sniffer task context.
void set_auto_stop_callback(std::function<void()> cb);

/// Add a transaction to the recording buffer (called from sniffer callback).
/// Thread-safe.  Auto-stops recording if the buffer is full.
void add(const sniffer::Transaction &txn);

/// Format a transaction as a JSONL line into the provided buffer.
/// Returns the number of bytes written.  Used by both in-memory
/// recording and web streaming.
int format_jsonl(const sniffer::Transaction &txn, char *buf, size_t buf_len);

/// Get the recorded JSONL data.  Returns a pointer to the internal buffer
/// and sets `len` to the number of bytes.  Caller must NOT free.
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

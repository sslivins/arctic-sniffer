#pragma once

#include "modbus_sniffer.h"
#include <cstdint>
#include <cstddef>

namespace recorder {

/// Start recording.  Clears any existing buffer.
void start();

/// Stop recording.
void stop();

/// Returns true if currently recording.
bool is_recording();

/// Add a transaction to the recording buffer (called from sniffer callback).
/// Thread-safe.
void add(const sniffer::Transaction &txn);

/// Get the recorded JSONL data.  Returns a pointer to the internal buffer
/// and sets `len` to the number of bytes.  Caller must NOT free.
/// Only valid while recording is stopped.
const char *get_data(size_t &len);

/// Get the number of recorded entries.
size_t get_entry_count();

/// Clear all recorded data and free memory.
void clear();

}  // namespace recorder

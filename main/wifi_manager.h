#pragma once

#include "esp_err.h"

namespace wifi {

/// Initialize WiFi in STA mode and connect using Kconfig credentials.
/// Also starts mDNS with the configured hostname.
/// Blocks until connected or fails after retries.
esp_err_t init();

/// Return true if currently connected to an AP.
bool is_connected();

/// Get the assigned IP address as a string (e.g. "192.168.1.42").
/// Returns empty string if not connected.
const char *get_ip();

}  // namespace wifi

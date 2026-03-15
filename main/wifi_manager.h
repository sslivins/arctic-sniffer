#pragma once

#include "esp_err.h"

namespace wifi {

/// Current WiFi operating mode.
enum class Mode {
    IDLE,           ///< Not started yet
    CONNECTING,     ///< Attempting STA connection
    CONNECTED,      ///< STA connected, IP acquired
    PROVISIONING,   ///< SoftAP captive portal active
};

/// Initialize WiFi subsystem:
///   1. Check NVS for stored SSID/password.
///   2. If found, attempt STA connection (blocking, with timeout).
///   3. If STA fails or no creds exist, start SoftAP provisioning.
/// Returns ESP_OK when STA connected,
/// ESP_ERR_NOT_FINISHED when provisioning is active.
esp_err_t init();

/// Current mode.
Mode get_mode();

/// True when STA is connected and has an IP.
bool is_connected();

/// STA IP address as string ("0.0.0.0" when not connected).
const char *get_ip();

/// SoftAP SSID (only meaningful when provisioning).
const char *get_ap_name();

/// Erase stored WiFi credentials from NVS.  Caller should reboot
/// afterward so the device re-enters provisioning mode.
esp_err_t erase_credentials();

}  // namespace wifi

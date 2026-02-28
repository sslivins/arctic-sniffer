#pragma once

#include "modbus_sniffer.h"
#include "esp_err.h"

namespace api {

/// Start the HTTP server (REST + WebSocket).
/// Must be called after wifi::init().
esp_err_t init();

/// Push a transaction to all connected WebSocket clients.
/// Called from the sniffer callback.  Thread-safe.
void broadcast_transaction(const sniffer::Transaction &txn);

}  // namespace api

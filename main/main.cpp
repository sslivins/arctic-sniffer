/// @file main.cpp
/// @brief Arctic Sniffer entry point.
///
/// Initialises WiFi, starts the HTTP/WebSocket server, then launches the
/// Modbus sniffer task.  Every decoded transaction is broadcast to WebSocket
/// clients and, when recording is active, appended to the recorder buffer.

#include "wifi_manager.h"
#include "api_server.h"
#include "modbus_sniffer.h"
#include "recorder.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

extern "C" void app_main()
{
    // --- NVS (required by WiFi) ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- WiFi ---
    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(wifi::init());
    ESP_LOGI(TAG, "WiFi connected — IP: %s", wifi::get_ip());

    // --- HTTP / WebSocket server ---
    ESP_ERROR_CHECK(api::init());
    ESP_LOGI(TAG, "HTTP server started");

    // --- Modbus sniffer ---
    sniffer::init([](const sniffer::Transaction &txn) {
        // Push to all WebSocket clients
        api::broadcast_transaction(txn);

        // Append to recording buffer when active
        if (recorder::is_recording()) {
            recorder::add(txn);
        }
    });
    ESP_LOGI(TAG, "Modbus sniffer running — listening on UART");
}

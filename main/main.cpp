/// @file main.cpp
/// @brief Arctic Sniffer entry point.
///
/// Initialises WiFi, display, starts the HTTP/WebSocket server, then launches
/// the Modbus sniffer task and display task.  Every decoded transaction is
/// broadcast to WebSocket clients and, when recording is active, appended to
/// the recorder buffer.  The front button toggles recording on/off (S3R only).

#include "wifi_manager.h"
#include "api_server.h"
#include "modbus_sniffer.h"
#include "recorder.h"
#include "display.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Display task — button polling (50 ms) + screen refresh (500 ms)
// ---------------------------------------------------------------------------

static void display_task(void *param)
{
    ESP_LOGI(TAG, "Display task started");
    int refresh_counter = 0;
    bool can_record = recorder::has_memory_recording();

    while (true) {
        bool provisioning = (wifi::get_mode() == wifi::Mode::PROVISIONING);

        // Poll button every 50 ms — only toggle recording when connected
        if (!provisioning && can_record && display::button_pressed()) {
            if (recorder::is_recording()) {
                recorder::stop();
                ESP_LOGI(TAG, "Recording stopped (button)");
            } else {
                recorder::clear();
                recorder::start();
                ESP_LOGI(TAG, "Recording started (button)");
            }
        }

        // Refresh screen every 500 ms (10 × 50 ms)
        if (++refresh_counter >= 10) {
            if (provisioning) {
                display::refresh_provisioning(wifi::get_ap_name());
            } else {
                display::refresh(
                    wifi::get_ip(),
                    recorder::is_recording(),
                    recorder::get_buffer_used(),
                    recorder::get_buffer_limit(),
                    recorder::get_entry_count(),
                    can_record
                );
            }
            refresh_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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

    // --- Display (init early so splash shows during WiFi connect) ---
    if (display::init() != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed — running headless");
    }

    // --- Recorder (probes PSRAM, allocates buffer on S3R) ---
    recorder::init();

    // --- WiFi ---
    ESP_LOGI(TAG, "Initializing WiFi...");
    esp_err_t wifi_ret = wifi::init();

    if (wifi_ret == ESP_OK) {
        // STA connected — start the main application
        ESP_LOGI(TAG, "WiFi connected — IP: %s", wifi::get_ip());

        // --- HTTP / WebSocket server ---
        ESP_ERROR_CHECK(api::init());
        ESP_LOGI(TAG, "HTTP server started");

        // --- Recorder auto-stop callback ---
        recorder::set_auto_stop_callback([]() {
            ESP_LOGW(TAG, "Recording auto-stopped (buffer full)");
        });

        // --- Modbus sniffer ---
        sniffer::init([](const sniffer::Transaction &txn) {
            // Push to all WebSocket clients
            api::broadcast_transaction(txn);

            // Append to recording buffer when active (S3R only)
            if (recorder::is_recording()) {
                recorder::add(txn);
            }
        });
        ESP_LOGI(TAG, "Modbus sniffer running — listening on UART");
    } else if (wifi_ret == ESP_ERR_NOT_FINISHED) {
        // Provisioning mode — captive portal is already running
        ESP_LOGI(TAG, "WiFi provisioning active — connect to '%s'",
                 wifi::get_ap_name());
    } else {
        ESP_LOGE(TAG, "WiFi failed — API will not be available");
    }

    // --- Display task ---
    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 2,
                            nullptr, 0);
}

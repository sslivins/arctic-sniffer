#include "wifi_manager.h"

#include <cstring>
#include <atomic>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

namespace wifi {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static EventGroupHandle_t s_wifi_events;
constexpr int CONNECTED_BIT = BIT0;
constexpr int FAIL_BIT      = BIT1;

static std::atomic<bool> s_connected{false};
static char s_ip_str[16] = {};
static int  s_retry_count = 0;
constexpr int MAX_RETRIES = 10;

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected.store(false, std::memory_order_relaxed);
        if (s_retry_count < MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retry %d/%d", s_retry_count, MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_events, FAIL_BIT);
            ESP_LOGE(TAG, "Connection failed after %d retries", MAX_RETRIES);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected.store(true, std::memory_order_relaxed);
        s_retry_count = 0;
        ESP_LOGI(TAG, "Connected — IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

// ---------------------------------------------------------------------------
// mDNS
// ---------------------------------------------------------------------------

static void start_mdns()
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_MDNS_HOSTNAME);
    mdns_instance_name_set("Arctic Sniffer");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", CONFIG_MDNS_HOSTNAME);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t init()
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr, nullptr));

    wifi_config_t sta_cfg = {};
    strncpy((char *)sta_cfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(sta_cfg.sta.ssid));
    strncpy((char *)sta_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to \"%s\"…", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (bits & CONNECTED_BIT) {
        start_mdns();
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return ESP_FAIL;
}

bool is_connected()
{
    return s_connected.load(std::memory_order_relaxed);
}

const char *get_ip()
{
    return s_ip_str;
}

}  // namespace wifi

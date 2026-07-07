/*
 * Arctic Sniffer
 * OTA Update Manager Implementation (ported from arctic-controller)
 */
#include "ota_manager.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <cJSON.h>

static const char* TAG = "ota_manager";

// OTA task stack size
#define OTA_TASK_STACK_SIZE 8192

// GitHub API URL for releases
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-sniffer/releases/latest"

// Firmware asset name published by create-release.yml. The release also
// contains bootloader.bin and partition-table.bin, so we must match the app
// image specifically — a generic ".bin" match could grab the wrong asset.
#define OTA_ASSET_NAME "arctic-sniffer.bin"

// Allowed URL prefixes for firmware updates (security)
#define ALLOWED_OTA_URL_PREFIX "https://github.com/sslivins/arctic-sniffer/"
#define ALLOWED_OTA_URL_PREFIX2 "https://objects.githubusercontent.com/"

bool ota_mgr_is_url_allowed(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        return false;
    }
    // Allow both GitHub repo URLs and GitHub's CDN (objects.githubusercontent.com)
    return strncmp(url, ALLOWED_OTA_URL_PREFIX, strlen(ALLOWED_OTA_URL_PREFIX)) == 0 ||
           strncmp(url, ALLOWED_OTA_URL_PREFIX2, strlen(ALLOWED_OTA_URL_PREFIX2)) == 0;
}

// Current OTA status
static ota_status_t ota_status = {
    .state = OTA_STATE_IDLE,
    .progress_percent = 0,
    .bytes_downloaded = 0,
    .total_bytes = 0,
    .error_msg = "",
    .current_version = "",
    .new_version = ""
};

// Cached release info
static ota_release_info_t release_info = {
    .update_available = false,
    .latest_version = "",
    .download_url = "",
    .release_notes = "",
    .published_at = ""
};

// URL for current update
static char update_url[256] = "";

// Mutex for status access
static SemaphoreHandle_t status_mutex = NULL;

// Forward declarations
static void ota_task(void* pvParameter);

bool ota_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA manager...");
    
    // Create mutex
    status_mutex = xSemaphoreCreateMutex();
    if (status_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }
    
    // Get current app version
    const esp_app_desc_t* app_desc = esp_app_get_description();
    if (app_desc) {
        strncpy(ota_status.current_version, app_desc->version, sizeof(ota_status.current_version) - 1);
        ESP_LOGI(TAG, "Current firmware version: %s", ota_status.current_version);
    }
    
    // Log partition info
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Running from partition: %s @ 0x%lx", running->label, running->address);
    }
    
    // Check if this is first boot after OTA
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "First boot after OTA - firmware pending verification");
        }
    }
    
    return true;
}

bool ota_mgr_start_update(const char* url)
{
    if (url == NULL || strlen(url) == 0) {
        ESP_LOGE(TAG, "Invalid URL");
        return false;
    }
    
    // Security: Only allow updates from official GitHub repository
    if (strncmp(url, ALLOWED_OTA_URL_PREFIX, strlen(ALLOWED_OTA_URL_PREFIX)) != 0) {
        ESP_LOGE(TAG, "URL not allowed: must start with %s", ALLOWED_OTA_URL_PREFIX);
        return false;
    }
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Check for any ongoing OTA operation (including file upload)
    if (ota_status.state == OTA_STATE_UPLOADING ||
        ota_status.state == OTA_STATE_DOWNLOADING || 
        ota_status.state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "OTA already in progress (state=%d)", ota_status.state);
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    // Store URL and reset status
    strncpy(update_url, url, sizeof(update_url) - 1);
    update_url[sizeof(update_url) - 1] = '\0';
    
    ota_status.state = OTA_STATE_DOWNLOADING;
    ota_status.progress_percent = 0;
    ota_status.bytes_downloaded = 0;
    ota_status.total_bytes = 0;
    ota_status.error_msg[0] = '\0';
    ota_status.new_version[0] = '\0';
    
    xSemaphoreGive(status_mutex);
    
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);
    
    // Create OTA task
    // Lower priority than LVGL (5) to avoid display glitches during flash writes
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Failed to start OTA task", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    return true;
}

ota_status_t ota_mgr_get_status(void)
{
    ota_status_t status;
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    memcpy(&status, &ota_status, sizeof(ota_status_t));
    xSemaphoreGive(status_mutex);
    return status;
}

bool ota_mgr_is_busy(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    bool busy = (ota_status.state == OTA_STATE_UPLOADING ||
                 ota_status.state == OTA_STATE_DOWNLOADING || 
                 ota_status.state == OTA_STATE_VERIFYING);
    xSemaphoreGive(status_mutex);
    return busy;
}

bool ota_mgr_try_lock_upload(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Check if any OTA operation is in progress
    if (ota_status.state == OTA_STATE_UPLOADING ||
        ota_status.state == OTA_STATE_DOWNLOADING ||
        ota_status.state == OTA_STATE_VERIFYING) {
        ESP_LOGW(TAG, "Cannot start upload: OTA already in progress (state=%d)", ota_status.state);
        xSemaphoreGive(status_mutex);
        return false;
    }
    
    // Acquire lock by setting state
    ota_status.state = OTA_STATE_UPLOADING;
    ota_status.progress_percent = 0;
    ota_status.error_msg[0] = '\0';
    
    xSemaphoreGive(status_mutex);
    ESP_LOGI(TAG, "Upload lock acquired");
    return true;
}

void ota_mgr_unlock_upload(void)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    
    // Only unlock if we're in uploading state (don't disturb other states)
    if (ota_status.state == OTA_STATE_UPLOADING) {
        ota_status.state = OTA_STATE_IDLE;
        ESP_LOGI(TAG, "Upload lock released");
    }
    
    xSemaphoreGive(status_mutex);
}

void ota_mgr_reboot(void)
{
    ESP_LOGI(TAG, "Rebooting to apply OTA update...");
    vTaskDelay(pdMS_TO_TICKS(500));  // Allow log to flush
    esp_restart();
}

void ota_mgr_mark_valid(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Marking current firmware as valid — rollback cancelled");
            esp_ota_mark_app_valid_cancel_rollback();
        } else {
            ESP_LOGI(TAG, "Firmware already validated (state=%d)", ota_state);
        }
    }
}

bool ota_mgr_is_pending_verify(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        return ota_state == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return false;
}

void ota_mgr_get_partition_info(char* label, uint32_t* address, uint32_t* size)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        if (label) {
            strncpy(label, running->label, 16);
        }
        if (address) {
            *address = running->address;
        }
        if (size) {
            *size = running->size;
        }
    }
}

// HTTP event handler for progress tracking
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP connected");
            break;
        case HTTP_EVENT_HEADER_SENT:
            break;
        case HTTP_EVENT_ON_HEADER:
            // Note: Don't parse Content-Length here - with partial_http_download enabled,
            // headers show chunk sizes (e.g., 8192) not actual firmware size.
            // We get the correct total from esp_https_ota_get_image_size() instead.
            ESP_LOGD(TAG, "Header: %s = %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP finished");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP disconnected");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void ota_task(void* pvParameter)
{
    ESP_LOGI(TAG, "OTA task started");
    
    // Reduce verbosity of certificate bundle logging
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    
    esp_err_t err;
    
    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = update_url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 60000;  // 60 second timeout for large files
    config.keep_alive_enable = true;
    config.buffer_size = 8192;  // Larger buffer for HTTPS TLS handshake
    config.buffer_size_tx = 4096;  // Larger TX buffer for GitHub
    
    // Check if HTTPS
    if (strncmp(update_url, "https://", 8) == 0) {
        // Use ESP's built-in certificate bundle for HTTPS verification
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = false;
        ESP_LOGI(TAG, "HTTPS: Using certificate bundle for verification");
    }
    
    // GitHub uses redirects for release downloads (to objects.githubusercontent.com)
    config.disable_auto_redirect = false;
    config.max_redirection_count = 10;
    
    // Perform OTA with partial download (handles redirects/chunked better)
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;
    ota_config.partial_http_download = true;
    ota_config.max_http_request_size = 64 * 1024;  // 64KB chunks for faster download
    
    esp_https_ota_handle_t https_ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                 "OTA begin failed: %s", esp_err_to_name(err));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Get new app info
    esp_app_desc_t new_app_info;
    err = esp_https_ota_get_img_desc(https_ota_handle, &new_app_info);
    if (err == ESP_OK) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        strncpy(ota_status.new_version, new_app_info.version, sizeof(ota_status.new_version) - 1);
        xSemaphoreGive(status_mutex);
        ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
    }
    
    // Get total image size from OTA handle
    // Note: GitHub uses chunked transfer encoding, so this may return 0
    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "Image size from server: %d bytes (0 means unknown/chunked)", image_size);
    
    // If image size unknown, use a reasonable estimate based on current firmware size
    // Sniffer app is ~0.9MB, so estimate 1.1MB for progress display
    int estimated_size = (image_size > 0) ? image_size : (1100 * 1024);
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.total_bytes = estimated_size;
    xSemaphoreGive(status_mutex);
    
    if (image_size <= 0) {
        ESP_LOGW(TAG, "Using estimated size: %d bytes (server didn't provide Content-Length)", estimated_size);
    }
    
    // Download and write firmware
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        // Update progress
        int image_len = esp_https_ota_get_image_len_read(https_ota_handle);
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.bytes_downloaded = image_len;
        if (ota_status.total_bytes > 0) {
            int progress = (image_len * 100) / ota_status.total_bytes;
            // Cap at 99% until download is complete (in case estimate was too small)
            ota_status.progress_percent = (progress > 99) ? 99 : progress;
        }
        xSemaphoreGive(status_mutex);
        
        // Log progress every 10%
        static int last_logged = -1;
        if (ota_status.progress_percent / 10 != last_logged) {
            last_logged = ota_status.progress_percent / 10;
            ESP_LOGI(TAG, "OTA progress: %d%% (%d bytes)", 
                     ota_status.progress_percent, image_len);
        }
        
        // Brief yield to let other tasks run
        vTaskDelay(1);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                 "Download failed: %s", esp_err_to_name(err));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Verify and finish
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_VERIFYING;
    ota_status.progress_percent = 100;
    xSemaphoreGive(status_mutex);
    
    ESP_LOGI(TAG, "Download complete, verifying...");
    
    if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
        ESP_LOGE(TAG, "Complete data was not received");
        esp_https_ota_abort(https_ota_handle);
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        strncpy(ota_status.error_msg, "Incomplete data received", sizeof(ota_status.error_msg));
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    err = esp_https_ota_finish(https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        
        xSemaphoreTake(status_mutex, portMAX_DELAY);
        ota_status.state = OTA_STATE_FAILED;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            strncpy(ota_status.error_msg, "Image validation failed", sizeof(ota_status.error_msg));
        } else {
            snprintf(ota_status.error_msg, sizeof(ota_status.error_msg), 
                     "OTA finish failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(status_mutex);
        vTaskDelete(NULL);
        return;
    }
    
    // Success!
    ESP_LOGI(TAG, "OTA update successful! Rebooting in 3 seconds...");
    
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    ota_status.state = OTA_STATE_READY_TO_REBOOT;
    xSemaphoreGive(status_mutex);
    
    // Give UI time to show "Update complete" message, then auto-reboot
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Rebooting to apply OTA update...");
    esp_restart();
    
    // Never reached
    vTaskDelete(NULL);
}

// Compare semantic versions (returns: -1 if v1<v2, 0 if equal, 1 if v1>v2)
int ota_mgr_compare_versions(const char* v1, const char* v2)
{
    int major1 = 0, minor1 = 0, patch1 = 0;
    int major2 = 0, minor2 = 0, patch2 = 0;
    
    // Skip 'v' prefix if present
    if (v1[0] == 'v' || v1[0] == 'V') v1++;
    if (v2[0] == 'v' || v2[0] == 'V') v2++;
    
    sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
    sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
    
    if (major1 != major2) return (major1 > major2) ? 1 : -1;
    if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
    if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;
    return 0;
}

// HTTP response buffer for GitHub API
static char* http_response_buffer = NULL;
static size_t http_response_len = 0;
static size_t http_response_capacity = 0;

static esp_err_t github_http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_buffer == NULL) {
                // Initial allocation
                http_response_capacity = 4096;
                http_response_buffer = (char*)malloc(http_response_capacity);
                http_response_len = 0;
            }
            // Grow buffer if needed
            if (http_response_len + evt->data_len >= http_response_capacity) {
                http_response_capacity = http_response_len + evt->data_len + 1024;
                http_response_buffer = (char*)realloc(http_response_buffer, http_response_capacity);
            }
            if (http_response_buffer) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool ota_mgr_check_github_releases(ota_release_info_t* info)
{
    ESP_LOGI(TAG, "Checking GitHub for updates...");
    
    // Reset release info
    memset(&release_info, 0, sizeof(release_info));
    
    // Free any previous response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;
    
    // Configure HTTP client for GitHub API
    esp_http_client_config_t config = {};
    config.url = GITHUB_API_URL;
    config.event_handler = github_http_event_handler;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }
    
    // GitHub API requires User-Agent header
    esp_http_client_set_header(client, "User-Agent", "arctic-sniffer");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "GitHub API request failed: %s (HTTP %d)", esp_err_to_name(err), status_code);
        if (http_response_buffer) {
            free(http_response_buffer);
            http_response_buffer = NULL;
        }
        return false;
    }
    
    ESP_LOGI(TAG, "Got GitHub response (%d bytes)", http_response_len);
    
    // Parse JSON response
    cJSON* root = cJSON_Parse(http_response_buffer);
    free(http_response_buffer);
    http_response_buffer = NULL;
    
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse GitHub JSON response");
        return false;
    }
    
    // Extract version tag
    cJSON* tag_name = cJSON_GetObjectItem(root, "tag_name");
    if (tag_name && cJSON_IsString(tag_name)) {
        strncpy(release_info.latest_version, tag_name->valuestring, sizeof(release_info.latest_version) - 1);
    }
    
    // Extract published date
    cJSON* published_at = cJSON_GetObjectItem(root, "published_at");
    if (published_at && cJSON_IsString(published_at)) {
        strncpy(release_info.published_at, published_at->valuestring, sizeof(release_info.published_at) - 1);
    }
    
    // Extract release notes (body), truncate if needed
    cJSON* body = cJSON_GetObjectItem(root, "body");
    if (body && cJSON_IsString(body)) {
        strncpy(release_info.release_notes, body->valuestring, sizeof(release_info.release_notes) - 1);
    }
    
    // Find the .bin asset in assets array
    cJSON* assets = cJSON_GetObjectItem(root, "assets");
    if (assets && cJSON_IsArray(assets)) {
        cJSON* asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON* name = cJSON_GetObjectItem(asset, "name");
            if (name && cJSON_IsString(name)) {
                // Match the app image specifically (not bootloader.bin /
                // partition-table.bin, which are also attached to the release).
                if (strcmp(name->valuestring, OTA_ASSET_NAME) == 0) {
                    cJSON* download_url = cJSON_GetObjectItem(asset, "browser_download_url");
                    if (download_url && cJSON_IsString(download_url)) {
                        strncpy(release_info.download_url, download_url->valuestring, sizeof(release_info.download_url) - 1);
                        ESP_LOGI(TAG, "Found firmware: %s", name->valuestring);
                        break;
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    // Compare versions
    if (strlen(release_info.latest_version) > 0 && strlen(ota_status.current_version) > 0) {
        int cmp = ota_mgr_compare_versions(release_info.latest_version, ota_status.current_version);
        release_info.update_available = (cmp > 0);
        ESP_LOGI(TAG, "Current: %s, Latest: %s, Update available: %s",
                 ota_status.current_version, release_info.latest_version,
                 release_info.update_available ? "YES" : "NO");
    }
    
    if (info) {
        memcpy(info, &release_info, sizeof(ota_release_info_t));
    }
    
    return true;
}

const ota_release_info_t* ota_mgr_get_release_info(void)
{
    return &release_info;
}

bool ota_mgr_start_github_update(void)
{
    if (strlen(release_info.download_url) == 0) {
        ESP_LOGE(TAG, "No download URL available - run check first");
        return false;
    }
    
    if (!release_info.update_available) {
        ESP_LOGW(TAG, "No update available");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting GitHub OTA update to %s", release_info.latest_version);
    return ota_mgr_start_update(release_info.download_url);
}

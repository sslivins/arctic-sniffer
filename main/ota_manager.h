/*
 * Arctic Sniffer
 * OTA Update Manager (ported from arctic-controller)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// OTA update states
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_CHECKING,     // Checking GitHub for updates
    OTA_STATE_UPLOADING,    // File upload in progress
    OTA_STATE_DOWNLOADING,  // URL download in progress
    OTA_STATE_VERIFYING,
    OTA_STATE_READY_TO_REBOOT,
    OTA_STATE_FAILED
} ota_state_t;

// OTA progress info
typedef struct {
    ota_state_t state;
    int progress_percent;       // 0-100
    size_t bytes_downloaded;
    size_t total_bytes;
    char error_msg[128];
    char current_version[32];
    char new_version[32];
} ota_status_t;

// GitHub release info
typedef struct {
    bool update_available;
    char latest_version[32];
    char download_url[256];
    char release_notes[512];
    char published_at[32];
} ota_release_info_t;

/**
 * @brief Initialize OTA manager
 * @return true on success
 */
bool ota_mgr_init(void);

/**
 * @brief Check if a URL is allowed for OTA updates
 * @param url URL to check
 * @return true if URL is allowed
 */
bool ota_mgr_is_url_allowed(const char* url);

/**
 * @brief Start OTA update from URL
 * @param url URL to firmware binary (http or https)
 * @return true if update started, false if already in progress or error
 */
bool ota_mgr_start_update(const char* url);

/**
 * @brief Get current OTA status
 * @return Current OTA status
 */
ota_status_t ota_mgr_get_status(void);

/**
 * @brief Check if OTA update is in progress
 * @return true if update in progress
 */
bool ota_mgr_is_busy(void);

/**
 * @brief Try to acquire upload lock
 * Prevents concurrent OTA updates
 * @return true if lock acquired, false if OTA already in progress
 */
bool ota_mgr_try_lock_upload(void);

/**
 * @brief Release upload lock
 * Call after upload completes (success or failure)
 */
void ota_mgr_unlock_upload(void);

/**
 * @brief Reboot to apply pending update
 * Should only be called when state is OTA_STATE_READY_TO_REBOOT
 */
void ota_mgr_reboot(void);

/**
 * @brief Mark current firmware as valid (rollback protection)
 * Call this after successful boot to prevent rollback.
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, new firmware boots in
 * ESP_OTA_IMG_PENDING_VERIFY state.  If this function is not called
 * before the next reboot, the bootloader rolls back to the previous
 * working partition.
 */
void ota_mgr_mark_valid(void);

/**
 * @brief Check if firmware is pending OTA verification
 * @return true if running partition is in PENDING_VERIFY state
 */
bool ota_mgr_is_pending_verify(void);

/**
 * @brief Get running partition info
 * @param label Buffer to store partition label (min 16 bytes)
 * @param address Pointer to store partition address (can be NULL)
 * @param size Pointer to store partition size (can be NULL)
 */
void ota_mgr_get_partition_info(char* label, uint32_t* address, uint32_t* size);

/**
 * @brief Check GitHub for available updates
 * @param info Pointer to store release info
 * @return true if check succeeded (doesn't mean update available)
 */
bool ota_mgr_check_github_releases(ota_release_info_t* info);

/**
 * @brief Get cached release info from last check
 * @return Pointer to cached release info (valid until next check)
 */
const ota_release_info_t* ota_mgr_get_release_info(void);

/**
 * @brief Compare two semantic version strings
 * @param v1 First version (e.g. "2.10.0" or "v2.10.0")
 * @param v2 Second version
 * @return -1 if v1<v2, 0 if equal, 1 if v1>v2
 */
int ota_mgr_compare_versions(const char* v1, const char* v2);

/**
 * @brief Start OTA update from GitHub release
 * Uses the URL from the last successful release check
 * @return true if update started
 */
bool ota_mgr_start_github_update(void);

#ifdef __cplusplus
}
#endif

/*
 * Display driver for M5Stack Atom S3 / S3R
 *
 * Drives the 128×128 GC9107 LCD via SPI and provides a simple
 * status UI with recording controls.  Also handles front-button
 * polling with debounce.
 */
#pragma once

#include "esp_err.h"
#include <cstddef>

namespace display {

/// Initialise LCD, backlight, button GPIO.
/// Safe to call on boards without a display — returns an error
/// but does not crash.
esp_err_t init();

/// Redraw the status screen (call periodically, e.g. every 500 ms).
/// Pass the IP address string and recording state for the UI.
/// has_psram controls whether recording controls are shown.
void refresh(const char *ip, bool recording, size_t rec_used, size_t rec_limit,
             size_t rec_entries, bool has_psram);

/// Poll the front button.  Returns true once per press (debounced).
bool button_pressed();

}  // namespace display

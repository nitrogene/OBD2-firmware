#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the LilyGO TTGO T-Display S3 ST7789 status display.
 * No-op returning ESP_OK when CONFIG_OBD2_VIRTUAL_BENCH is disabled.
 */
esp_err_t virtual_bench_ui_init(void);

/**
 * @brief Update the last AT/OBD command/response string shown on the TFT.
 */
void virtual_bench_ui_set_last_cmd(const char *text);

#ifdef __cplusplus
}
#endif

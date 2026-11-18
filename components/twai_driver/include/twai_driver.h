#ifndef TWAI_DRIVER_H
#define TWAI_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TWAI_DRV_BITRATE_500K = 500000,
    TWAI_DRV_BITRATE_250K = 250000,
} twai_drv_bitrate_t;

typedef enum {
    TWAI_DRV_MODE_NORMAL = 0,
    TWAI_DRV_MODE_LISTEN_ONLY,
    TWAI_DRV_MODE_NO_ACK,
} twai_drv_mode_t;

typedef struct {
    uint32_t acceptance_code;
    uint32_t acceptance_mask;
    bool single_filter;
} twai_drv_filter_t;

typedef struct {
    uint32_t id;
    bool extd;
    bool rtr;
    uint8_t dlc;
    uint8_t data[8];
} twai_drv_frame_t;

/**
 * @brief Install and start the ESP32-S3 TWAI driver on BSP_PIN_TWAI_TX/RX.
 *
 * @param bitrate 500 kbps or 250 kbps.
 * @param mode    Normal, Listen-Only (probe/sniffer), or No-Ack.
 * @param filter  Optional hardware acceptance filter (NULL = accept all).
 */
esp_err_t twai_drv_init(twai_drv_bitrate_t bitrate, twai_drv_mode_t mode,
                        const twai_drv_filter_t *filter);

/**
 * @brief Stop and uninstall the TWAI driver.
 */
esp_err_t twai_drv_deinit(void);

/**
 * @brief Transmit a single CAN 2.0A/2.0B frame.
 */
esp_err_t twai_drv_send(const twai_drv_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief Receive a single CAN 2.0A/2.0B frame.
 */
esp_err_t twai_drv_receive(twai_drv_frame_t *out_frame, uint32_t timeout_ms);

/**
 * @brief Probe CAN bus bitrate (500k then 250k) in LISTEN_ONLY mode (no TX/ACK).
 */
esp_err_t twai_drv_probe_bitrate(uint32_t per_rate_timeout_ms, twai_drv_bitrate_t *out_bitrate);

/**
 * @brief Check TWAI alerts/status and initiate Bus-Off recovery if needed.
 */
esp_err_t twai_drv_check_and_recover(bool *out_bus_off_recovered);

#ifdef __cplusplus
}
#endif

#endif /* TWAI_DRIVER_H */

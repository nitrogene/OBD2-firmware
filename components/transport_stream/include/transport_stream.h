#ifndef TRANSPORT_STREAM_H
#define TRANSPORT_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRANSPORT_ID_NONE = 0,
    TRANSPORT_ID_USB,
    TRANSPORT_ID_BLE,
    TRANSPORT_ID_WIFI,
} transport_id_t;

typedef int (*transport_tx_fn_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * @brief Initialize static ring-buffered transport stream (zero malloc on hot paths).
 */
esp_err_t transport_stream_init(uint32_t lock_timeout_ms);

/**
 * @brief Register a TX callback for a physical transport (USB, BLE, Wi-Fi).
 */
esp_err_t transport_stream_register_tx(transport_id_t id, transport_tx_fn_t tx_fn, void *ctx);

/**
 * @brief Feed received bytes from a transport into the unified RX ring buffer.
 *
 * Acquires the single-active-transport lock if free or timed out.
 * Bytes from non-active transports are dropped while another transport holds the lock.
 */
esp_err_t transport_stream_feed_rx(transport_id_t id, const uint8_t *data, size_t len);

/**
 * @brief Release the active transport lock (e.g., on BLE/TCP disconnect).
 */
void transport_stream_release(transport_id_t id);

/**
 * @brief Read bytes from the unified RX ring buffer (called by parser task).
 */
size_t transport_stream_read(uint8_t *out_buf, size_t max_len, uint32_t timeout_ms);

/**
 * @brief Write response bytes to whichever transport currently holds the lock.
 */
esp_err_t transport_stream_write(const uint8_t *data, size_t len);

/**
 * @brief Return the currently active transport ID.
 */
transport_id_t transport_stream_active_id(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_STREAM_H */

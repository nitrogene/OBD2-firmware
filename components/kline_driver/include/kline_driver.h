#ifndef KLINE_DRIVER_H
#define KLINE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "kline_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KLINE_BAUD_RATE 10400

typedef struct {
    uint16_t p1_max_ms;      /**< Inter-byte ECU response max (default 20 ms) */
    uint16_t p2_max_ms;      /**< Tester request -> ECU response max (default 50 ms) */
    uint16_t p3_min_ms;      /**< Min idle before next tester request (default 55 ms) */
    uint16_t p4_min_ms;      /**< Inter-byte tester TX spacing (default 5 ms) */
    uint32_t keepalive_ms;   /**< Periodic TesterPresent interval (default 2000 ms) */
} kline_timing_cfg_t;

/**
 * @brief Initialize K-Line UART1 (10,400 baud 8N1) on BSP_PIN_KLINE_TX/RX.
 */
esp_err_t kline_drv_init(const kline_timing_cfg_t *timings);

/**
 * @brief Uninstall K-Line UART1 driver.
 */
esp_err_t kline_drv_deinit(void);

/**
 * @brief Perform 5-Baud Slow Init (bit-bang IO5) and key-byte handshake.
 */
esp_err_t kline_drv_slow_init(uint8_t target_addr,
                              kline_proto_type_t *out_proto,
                              uint8_t *out_kb1,
                              uint8_t *out_kb2);

/**
 * @brief Perform ISO 14230-2 Fast-Init (25 ms T_inil / 25 ms T_wup) + StartCommunication.
 */
esp_err_t kline_drv_fast_init(uint8_t target_addr, uint8_t source_addr);

/**
 * @brief Transmit a K-Line frame with local echo verification, then read ECU response.
 */
esp_err_t kline_drv_send_recv(const uint8_t *tx_frame,
                              size_t tx_len,
                              uint8_t *rx_buf,
                              size_t max_rx,
                              size_t *out_rx_len,
                              uint32_t timeout_ms);

/**
 * @brief Send periodic keep-alive (TesterPresent 0x3E) if session is idle.
 */
esp_err_t kline_drv_keepalive_tick(uint8_t target_addr, uint8_t source_addr);

#ifdef __cplusplus
}
#endif

#endif /* KLINE_DRIVER_H */

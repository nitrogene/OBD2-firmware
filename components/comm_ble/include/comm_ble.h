#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default BLE advertised device name when not overridden in NVS.
 */
#define COMM_BLE_DEFAULT_NAME "OBD2-ELM327"

/**
 * @brief Initialize and start the NimBLE GATT UART server (FFF0 + FFE0 alias).
 *
 * Registers TX callback on transport_stream (TRANSPORT_ID_BLE), loads
 * configurable device name from NVS ("comm_ble" / "name"), enables bonding
 * and MTU negotiation, and starts advertising.
 *
 * @return ESP_OK on success, or an ESP_ERR_* code on failure.
 */
esp_err_t comm_ble_init(void);

/**
 * @brief Update the BLE device name in NVS and GAP service.
 *
 * @param name Null-terminated device name (1..31 bytes).
 * @return ESP_OK on success, or an ESP_ERR_* code on failure.
 */
esp_err_t comm_ble_set_name(const char *name);

/**
 * @brief Query whether a BLE central is currently connected.
 */
bool comm_ble_is_connected(void);

/**
 * @brief Get the negotiated ATT MTU for the active connection (23 if disconnected).
 */
uint16_t comm_ble_get_mtu(void);

#ifdef __cplusplus
}
#endif

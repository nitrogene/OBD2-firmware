#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default SoftAP SSID when not configured in NVS. */
#define COMM_WIFI_DEFAULT_SSID "OBD2-Scanner"
/** @brief Default SoftAP password (open if empty, or >= 8 chars for WPA2). */
#define COMM_WIFI_DEFAULT_PASS "12345678"
/** @brief ELM327 Wi-Fi standard TCP port. */
#define COMM_WIFI_TCP_PORT     35000

/**
 * @brief Initialize Wi-Fi SoftAP (192.168.0.10) and ELM327 TCP server (port 35000).
 *
 * Loads SSID/password from NVS ("comm_wifi" / "ssid", "pass"), configures
 * SoftAP static IP 192.168.0.10/24, registers TRANSPORT_ID_WIFI TX callback
 * on transport_stream, and starts the TCP listener task.
 *
 * @return ESP_OK on success, or an ESP_ERR_* code on failure.
 */
esp_err_t comm_wifi_init(void);

/**
 * @brief Store SoftAP SSID and password in NVS (applied on next boot/init).
 *
 * @param ssid Null-terminated SSID (1..32 chars).
 * @param pass Null-terminated password (0 for open, or 8..63 chars for WPA2).
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG / NVS error.
 */
esp_err_t comm_wifi_set_ap_credentials(const char *ssid, const char *pass);

/**
 * @brief Store optional STA SSID and password in NVS (enables AP+STA on next boot).
 * Pass empty ssid ("") to disable STA mode.
 */
esp_err_t comm_wifi_set_sta_credentials(const char *ssid, const char *pass);

/**
 * @brief Query whether a TCP client is currently connected to port 35000.
 */
bool comm_wifi_is_client_connected(void);

#ifdef __cplusplus
}
#endif

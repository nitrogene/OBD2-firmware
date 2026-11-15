#include "twai_driver.h"

#include <string.h>
#include "bsp_pins.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "twai_driver";

static bool s_installed = false;

static twai_mode_t to_idf_mode(twai_drv_mode_t mode)
{
    switch (mode) {
    case TWAI_DRV_MODE_LISTEN_ONLY:
        return TWAI_MODE_LISTEN_ONLY;
    case TWAI_DRV_MODE_NO_ACK:
        return TWAI_MODE_NO_ACK;
    case TWAI_DRV_MODE_NORMAL:
    default:
        return TWAI_MODE_NORMAL;
    }
}

static twai_timing_config_t to_idf_timing(twai_drv_bitrate_t bitrate)
{
    if (bitrate == TWAI_DRV_BITRATE_250K) {
        twai_timing_config_t cfg = TWAI_TIMING_CONFIG_250KBITS();
        return cfg;
    }
    twai_timing_config_t cfg = TWAI_TIMING_CONFIG_500KBITS();
    return cfg;
}

esp_err_t twai_drv_init(twai_drv_bitrate_t bitrate,
                        twai_drv_mode_t mode,
                        const twai_drv_filter_t *filter)
{
    if (s_installed) {
        ESP_RETURN_ON_ERROR(twai_drv_deinit(), TAG, "re-init deinit failed");
    }

    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
        BSP_PIN_TWAI_TX, BSP_PIN_TWAI_RX, to_idf_mode(mode));
    g_cfg.rx_queue_len = 32;
    g_cfg.tx_queue_len = 16;
    g_cfg.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                           TWAI_ALERT_ERR_PASS | TWAI_ALERT_RX_DATA;

    const twai_timing_config_t t_cfg = to_idf_timing(bitrate);
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (filter != NULL) {
        f_cfg.acceptance_code = filter->acceptance_code;
        f_cfg.acceptance_mask = filter->acceptance_mask;
        f_cfg.single_filter = filter->single_filter;
    }

    ESP_RETURN_ON_ERROR(twai_driver_install(&g_cfg, &t_cfg, &f_cfg), TAG, "twai_driver_install failed");
    esp_err_t start_err = twai_start();
    if (start_err != ESP_OK) {
        (void)twai_driver_uninstall();
        return start_err;
    }

    s_installed = true;
    ESP_LOGI(TAG, "TWAI started (%d bps, mode=%d)", (int)bitrate, (int)mode);
    return ESP_OK;
}

esp_err_t twai_drv_deinit(void)
{
    if (!s_installed) {
        return ESP_OK;
    }
    (void)twai_stop();
    esp_err_t err = twai_driver_uninstall();
    s_installed = false;
    return err;
}

esp_err_t twai_drv_send(const twai_drv_frame_t *frame, uint32_t timeout_ms)
{
    if (!s_installed || frame == NULL || frame->dlc > 8U) {
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t msg = {
        .identifier = frame->id,
        .extd = frame->extd ? 1U : 0U,
        .rtr = frame->rtr ? 1U : 0U,
        .data_length_code = frame->dlc,
    };
    memcpy(msg.data, frame->data, frame->dlc);

    return twai_transmit(&msg, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t twai_drv_receive(twai_drv_frame_t *out_frame, uint32_t timeout_ms)
{
    if (!s_installed || out_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    twai_message_t msg = {0};
    esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }

    out_frame->id = msg.identifier;
    out_frame->extd = (msg.extd != 0U);
    out_frame->rtr = (msg.rtr != 0U);
    out_frame->dlc = (msg.data_length_code <= 8U) ? msg.data_length_code : 8U;
    memcpy(out_frame->data, msg.data, out_frame->dlc);
    return ESP_OK;
}

esp_err_t twai_drv_probe_bitrate(uint32_t per_rate_timeout_ms,
                                 twai_drv_bitrate_t *out_bitrate)
{
    if (out_bitrate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const twai_drv_bitrate_t candidates[2] = {
        TWAI_DRV_BITRATE_500K,
        TWAI_DRV_BITRATE_250K,
    };

    for (size_t i = 0; i < 2; ++i) {
        ESP_RETURN_ON_ERROR(
            twai_drv_init(candidates[i], TWAI_DRV_MODE_LISTEN_ONLY, NULL),
            TAG,
            "probe init failed");

        twai_drv_frame_t frame = {0};
        esp_err_t rx_err = twai_drv_receive(&frame, per_rate_timeout_ms);
        (void)twai_drv_deinit();

        if (rx_err == ESP_OK) {
            *out_bitrate = candidates[i];
            ESP_LOGI(TAG, "Bitrate probe detected %d bps", (int)candidates[i]);
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t twai_drv_check_and_recover(bool *out_bus_off_recovered)
{
    if (!s_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_bus_off_recovered != NULL) {
        *out_bus_off_recovered = false;
    }

    uint32_t alerts = 0;
    esp_err_t err = twai_read_alerts(&alerts, 0);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return err;
    }

    if ((alerts & TWAI_ALERT_BUS_OFF) != 0U) {
        ESP_LOGW(TAG, "TWAI Bus-Off detected; initiating recovery");
        ESP_RETURN_ON_ERROR(twai_initiate_recovery(), TAG, "twai_initiate_recovery failed");
    }
    if ((alerts & TWAI_ALERT_BUS_RECOVERED) != 0U) {
        ESP_LOGI(TAG, "TWAI Bus-Off recovered; restarting");
        ESP_RETURN_ON_ERROR(twai_start(), TAG, "twai_start after recovery failed");
        if (out_bus_off_recovered != NULL) {
            *out_bus_off_recovered = true;
        }
    }
    return ESP_OK;
}

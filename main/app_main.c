#include "bsp.h"
#include "cli_commands.h"
#include "comm_ble.h"
#include "comm_wifi.h"
#include "ecu_simulator.h"
#include "elm327_emu.h"
#include "power_mgmt.h"
#include "transport_stream.h"
#include "virtual_bench_ui.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

static elm327_ctx_t s_elm_ctx;

static void elm_factory_reset_cb(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "Factory reset requested via AT+FACTORY");
    (void)nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void elm_write_bytes_cb(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    (void)transport_stream_write(data, len);
}

static uint32_t elm_get_vbat_mv_cb(void *ctx)
{
    (void)ctx;
    power_status_t st;
    if (power_mgmt_get_status(&st) == ESP_OK) {
        return st.vbat_mv;
    }
    return 0U;
}

static elm327_bus_status_t elm_send_obd_req_cb(uint32_t tx_header,
                                               uint8_t protocol,
                                               bool extended_mode,
                                               const uint8_t *req,
                                               size_t req_len,
                                               uint8_t *out_resp,
                                               size_t max_resp,
                                               size_t *out_resp_len,
                                               uint32_t *out_rx_id,
                                               void *ctx)
{
    (void)tx_header;
    (void)protocol;
    (void)ctx;

    int rc = ecu_sim_handle_request(ecu_simulator_state(),
                                    extended_mode,
                                    req,
                                    req_len,
                                    out_resp,
                                    max_resp,
                                    out_resp_len);
    if (rc == 0) {
        if (out_rx_id != NULL) {
            *out_rx_id = 0x7E8U;
        }
        return ELM_STATUS_OK;
    }
    return ELM_STATUS_NO_DATA;
}

static void elm327_host_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[128];

    for (;;) {
        size_t n = transport_stream_read(rx_buf, sizeof(rx_buf), 100U);
        if (n > 0U) {
            elm327_feed_bytes(&s_elm_ctx, rx_buf, n);
            if (s_elm_ctx.last_cmd[0] != '\0') {
                virtual_bench_ui_set_last_cmd(s_elm_ctx.last_cmd);
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Booting OBD2-firmware");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(bsp_init());
    bsp_led_set_pattern(BSP_LED_BREATHING);
    ESP_ERROR_CHECK(power_mgmt_init());
    ESP_ERROR_CHECK(transport_stream_init(5000U));
    ESP_ERROR_CHECK(ecu_simulator_init());
    ESP_ERROR_CHECK(virtual_bench_ui_init());

    elm327_callbacks_t elm_cb = {
        .write_bytes = elm_write_bytes_cb,
        .get_vbat_mv = elm_get_vbat_mv_cb,
        .send_obd_req = elm_send_obd_req_cb,
        .set_protocol = NULL,
        .factory_reset = elm_factory_reset_cb,
        .user_ctx = NULL,
    };
    elm327_init(&s_elm_ctx, &elm_cb);

    ESP_ERROR_CHECK(comm_ble_init());
    ESP_ERROR_CHECK(comm_wifi_init());

    BaseType_t ok = xTaskCreate(elm327_host_task, "elm327_host", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create elm327_host task");
    }

    ESP_ERROR_CHECK(cli_start());
}

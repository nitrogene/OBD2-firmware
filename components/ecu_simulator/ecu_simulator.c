#include "ecu_simulator.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kline_driver.h"
#include "kline_proto.h"
#include "twai_driver.h"

static const char *TAG = "ecu_sim";

static ecu_sim_state_t s_sim;
static volatile bool s_bus_running = false;
static TaskHandle_t s_can_task = NULL;
static TaskHandle_t s_kline_task = NULL;

static void ecu_sim_send_can_obd_resp(const uint8_t *resp, size_t resp_len)
{
    if (resp_len <= 7U) {
        twai_drv_frame_t tx = {
            .id = 0x7E8U,
            .extd = false,
            .rtr = false,
            .dlc = 8U,
        };
        memset(tx.data, 0xCCU, sizeof(tx.data));
        tx.data[0] = (uint8_t)resp_len;
        memcpy(&tx.data[1], resp, resp_len);
        (void)twai_drv_send(&tx, 20U);
        return;
    }

    /* Multi-frame ISO-TP First Frame + Consecutive Frames (best-effort) */
    twai_drv_frame_t ff = {
        .id = 0x7E8U,
        .extd = false,
        .rtr = false,
        .dlc = 8U,
    };
    ff.data[0] = (uint8_t)(0x10U | ((resp_len >> 8) & 0x0FU));
    ff.data[1] = (uint8_t)(resp_len & 0xFFU);
    memcpy(&ff.data[2], resp, 6U);
    (void)twai_drv_send(&ff, 20U);

    vTaskDelay(pdMS_TO_TICKS(5));
    size_t offset = 6U;
    uint8_t sn = 1U;
    while (offset < resp_len) {
        twai_drv_frame_t cf = {
            .id = 0x7E8U,
            .extd = false,
            .rtr = false,
            .dlc = 8U,
        };
        memset(cf.data, 0xCCU, sizeof(cf.data));
        cf.data[0] = (uint8_t)(0x20U | (sn & 0x0FU));
        size_t chunk = resp_len - offset;
        if (chunk > 7U) {
            chunk = 7U;
        }
        memcpy(&cf.data[1], &resp[offset], chunk);
        (void)twai_drv_send(&cf, 20U);
        offset += chunk;
        sn++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void ecu_sim_can_task(void *arg)
{
    (void)arg;
    if (twai_drv_init(TWAI_DRV_BITRATE_500K, TWAI_DRV_MODE_NORMAL, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TWAI for ECU simulator");
        s_can_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "CAN ECU simulator started (500 kbps, 0x7E8 responder + cyclic 0x280/0x316)");
    TickType_t last_cyclic = xTaskGetTickCount();

    while (s_bus_running) {
        twai_drv_frame_t rx;
        if (twai_drv_receive(&rx, 20U) == ESP_OK) {
            if (!rx.extd && !rx.rtr && (rx.id == 0x7DFU || rx.id == 0x7E0U) && rx.dlc >= 2U) {
                uint8_t pci = rx.data[0];
                if ((pci & 0xF0U) == 0x00U && pci >= 1U && pci <= 7U) {
                    uint8_t resp[256];
                    size_t resp_len = 0U;
                    if (ecu_sim_handle_request(&s_sim, true, &rx.data[1], pci, resp, sizeof(resp), &resp_len) == 0 &&
                        resp_len > 0U) {
                        ecu_sim_send_can_obd_resp(resp, resp_len);
                    }
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_cyclic) >= pdMS_TO_TICKS(100)) {
            last_cyclic = now;
            twai_drv_frame_t f_rpm = {
                .id = 0x280U,
                .extd = false,
                .rtr = false,
                .dlc = 8U,
                .data = {
                    s_sim.load_pct,
                    s_sim.throttle_pct,
                    (uint8_t)((s_sim.rpm * 4U) & 0xFFU),
                    (uint8_t)(((s_sim.rpm * 4U) >> 8) & 0xFFU),
                    0, 0, 0, 0
                },
            };
            (void)twai_drv_send(&f_rpm, 5U);

            twai_drv_frame_t f_spd = {
                .id = 0x316U,
                .extd = false,
                .rtr = false,
                .dlc = 8U,
                .data = {
                    s_sim.speed_kmh,
                    (uint8_t)(s_sim.coolant_c + 40),
                    0, 0, 0, 0, 0, 0
                },
            };
            (void)twai_drv_send(&f_spd, 5U);
        }
    }

    (void)twai_drv_deinit();
    s_can_task = NULL;
    vTaskDelete(NULL);
}

static void ecu_sim_kline_task(void *arg)
{
    (void)arg;
    if (kline_drv_init(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init K-Line for ECU simulator");
        s_kline_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "K-Line ECU simulator started (10,400 baud)");
    uint8_t rx_buf[128];
    uint8_t tx_buf[128];

    while (s_bus_running) {
        size_t rx_len = 0U;
        if (kline_drv_send_recv(NULL, 0U, rx_buf, sizeof(rx_buf), &rx_len, 100U) == ESP_OK && rx_len >= 5U) {
            kline_frame_t frame;
            if (kline_parse_frame(rx_buf, rx_len, &frame) == 0) {
                uint8_t resp_payload[112];
                size_t resp_payload_len = 0U;

                if (frame.payload_len == 1U && frame.payload[0] == 0x81U) {
                    /* KWP2000 StartCommunication positive response */
                    resp_payload[0] = 0xC1U;
                    resp_payload[1] = 0xEFU;
                    resp_payload[2] = 0x8FU;
                    resp_payload_len = 3U;
                } else {
                    (void)ecu_sim_handle_request(&s_sim, true, frame.payload, frame.payload_len,
                                                 resp_payload, sizeof(resp_payload), &resp_payload_len);
                }

                if (resp_payload_len > 0U && resp_payload_len <= 63U) {
                    int tx_len = kline_build_kwp_frame(0x80U, frame.source, frame.target,
                                                       resp_payload, (uint8_t)resp_payload_len,
                                                       tx_buf, sizeof(tx_buf));
                    if (tx_len > 0) {
                        size_t dummy = 0U;
                        (void)kline_drv_send_recv(tx_buf, (size_t)tx_len, NULL, 0U, &dummy, 20U);
                    }
                }
            }
        }
    }

    (void)kline_drv_deinit();
    s_kline_task = NULL;
    vTaskDelete(NULL);
}

ecu_sim_state_t *ecu_simulator_state(void)
{
    return &s_sim;
}

esp_err_t ecu_simulator_init(void)
{
    ecu_sim_init(&s_sim);
    return ESP_OK;
}

esp_err_t ecu_simulator_start_bus(bool enable_can, bool enable_kline)
{
    if (s_bus_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enable_can && !enable_kline) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bus_running = true;
    if (enable_can) {
        if (xTaskCreate(ecu_sim_can_task, "ecu_sim_can", 4096, NULL, 5, &s_can_task) != pdPASS) {
            s_bus_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    if (enable_kline) {
        if (xTaskCreate(ecu_sim_kline_task, "ecu_sim_kline", 4096, NULL, 5, &s_kline_task) != pdPASS) {
            s_bus_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void ecu_simulator_stop_bus(void)
{
    s_bus_running = false;
    for (int i = 0; i < 30 && (s_can_task != NULL || s_kline_task != NULL); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool ecu_simulator_is_bus_running(void)
{
    return s_bus_running;
}

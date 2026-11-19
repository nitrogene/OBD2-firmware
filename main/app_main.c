#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "cli_commands.h"
#include "comm_ble.h"
#include "comm_wifi.h"
#include "ecu_simulator.h"
#include "elm327_emu.h"
#include "isotp.h"
#include "kline_driver.h"
#include "kline_proto.h"
#include "obd2_autodetect.h"
#include "obd2_codec.h"
#include "power_mgmt.h"
#include "transport_stream.h"
#include "twai_driver.h"
#include "virtual_bench_ui.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

static elm327_ctx_t s_elm_ctx;
#ifndef CONFIG_OBD2_VIRTUAL_BENCH
static obd2_proto_id_t s_active_proto = OBD2_PROTO_AUTO;
static isotp_link_t s_isotp_link;
#endif

static void elm_factory_reset_cb(void *ctx) {
    (void)ctx;
    ESP_LOGW(TAG, "Factory reset requested via AT+FACTORY");
    (void)nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

#ifdef CONFIG_OBD2_VIRTUAL_BENCH
static void vbench_log_wire(const char *dir, const uint8_t *data, size_t len) {
    char esc[192];
    size_t pos = 0U;
    for (size_t i = 0U; i < len && pos + 3U < sizeof(esc); ++i) {
        uint8_t c = data[i];
        if (c == '\r') {
            esc[pos++] = '\\';
            esc[pos++] = 'r';
        } else if (c == '\n') {
            esc[pos++] = '\\';
            esc[pos++] = 'n';
        } else if (c >= 0x20U && c <= 0x7EU) {
            esc[pos++] = (char)c;
        } else {
            esc[pos++] = '.';
        }
    }
    esc[pos] = '\0';
    ESP_LOGI(TAG, "ELM %s (%uB): %s", dir, (unsigned)len, esc);
}
#endif

static void elm_write_bytes_cb(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
#ifdef CONFIG_OBD2_VIRTUAL_BENCH
    vbench_log_wire("TX", data, len);
#endif
    (void)transport_stream_write(data, len);
}

static uint32_t elm_get_vbat_mv_cb(void *ctx) {
    (void)ctx;
    power_status_t st;
    if (power_mgmt_get_status(&st) == ESP_OK) {
        return st.vbat_mv;
    }
    return 0U;
}

static int elm_sim_cmd_cb(const char *kv_upper, void *user_ctx) {
    (void)user_ctx;
    if (kv_upper == NULL) {
        return -1;
    }

    ecu_sim_state_t *sim = ecu_simulator_state();
    if (strncmp(kv_upper, "RPM=", 4) == 0) {
        long v = strtol(kv_upper + 4, NULL, 10);
        if (v < 0 || v > 16383) {
            return -1;
        }
        ecu_simulator_lock();
        ecu_sim_set_rpm(sim, (uint16_t)v);
        ecu_simulator_unlock();
        return 0;
    }
    if (strncmp(kv_upper, "SPD=", 4) == 0) {
        long v = strtol(kv_upper + 4, NULL, 10);
        if (v < 0 || v > 255) {
            return -1;
        }
        ecu_simulator_lock();
        ecu_sim_set_speed(sim, (uint8_t)v);
        ecu_simulator_unlock();
        return 0;
    }
    if (strncmp(kv_upper, "CLT=", 4) == 0) {
        long v = strtol(kv_upper + 4, NULL, 10);
        if (v < -40 || v > 215) {
            return -1;
        }
        ecu_simulator_lock();
        ecu_sim_set_coolant(sim, (int16_t)v);
        ecu_simulator_unlock();
        return 0;
    }
    if (strncmp(kv_upper, "VBAT=", 5) == 0) {
        long v = strtol(kv_upper + 5, NULL, 10);
        if (v < 0 || v > 24000) {
            return -1;
        }
        bsp_vbat_set_sim_mv((uint32_t)v);
        ecu_simulator_lock();
        sim->vbat_mv = (uint32_t)v;
        ecu_simulator_unlock();
        return 0;
    }
    if (strncmp(kv_upper, "DTC=", 4) == 0) {
        const char *dtc = kv_upper + 4;
        ecu_simulator_lock();
        int rc = 0;
        if (strcmp(dtc, "CLEAR") == 0) {
            ecu_sim_clear_dtcs(sim);
        } else {
            rc = ecu_sim_inject_dtc(sim, dtc);
        }
        ecu_simulator_unlock();
        return rc;
    }
    if (strcmp(kv_upper, "START") == 0) {
        return (ecu_simulator_start_bus(true, true) == ESP_OK) ? 0 : -1;
    }
    if (strcmp(kv_upper, "STOP") == 0) {
        ecu_simulator_stop_bus();
        return 0;
    }
    return -1;
}

#ifndef CONFIG_OBD2_VIRTUAL_BENCH
static int isotp_can_tx_cb(uint32_t can_id, bool extd, const uint8_t *data, uint8_t dlc,
                           void *user_ctx) {
    (void)user_ctx;
    twai_drv_frame_t frame = {
        .id = can_id,
        .extd = extd,
        .rtr = false,
        .dlc = dlc,
    };
    if (dlc > 0U && data != NULL) {
        memcpy(frame.data, data, dlc);
    }
    return (twai_drv_send(&frame, 50U) == ESP_OK) ? 0 : -1;
}

static uint32_t isotp_get_ms_cb(void *user_ctx) {
    (void)user_ctx;
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static esp_err_t activate_physical_protocol(obd2_proto_id_t proto) {
    switch (proto) {
    case OBD2_PROTO_CAN_11B_500K:
    case OBD2_PROTO_CAN_29B_500K:
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            twai_drv_init(TWAI_DRV_BITRATE_500K, TWAI_DRV_MODE_NORMAL, NULL));
        s_active_proto = proto;
        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ESP_OK;

    case OBD2_PROTO_CAN_11B_250K:
    case OBD2_PROTO_CAN_29B_250K:
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            twai_drv_init(TWAI_DRV_BITRATE_250K, TWAI_DRV_MODE_NORMAL, NULL));
        s_active_proto = proto;
        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ESP_OK;

    case OBD2_PROTO_KWP_FAST:
        if (kline_drv_fast_init(0x33U, 0xF1U) != ESP_OK) {
            /* Keep UART1 initialized even if ECU did not answer 0x81 yet */
            (void)kline_drv_init(NULL);
        }
        s_active_proto = proto;
        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ESP_OK;

    case OBD2_PROTO_KWP_5BAUD:
    case OBD2_PROTO_ISO9141_2: {
        kline_proto_type_t kb_proto = KLINE_PROTO_UNKNOWN;
        if (kline_drv_slow_init(0x33U, &kb_proto, NULL, NULL) != ESP_OK) {
            (void)kline_drv_init(NULL);
        }
        s_active_proto = proto;
        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ESP_OK;
    }

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static int elm_set_protocol_cb(uint8_t protocol, void *ctx) {
    (void)ctx;
    if (ecu_simulator_is_bus_running()) {
        return 0;
    }
    if (protocol == (uint8_t)OBD2_PROTO_AUTO) {
        s_active_proto = OBD2_PROTO_AUTO;
        return 0;
    }
    return (activate_physical_protocol((obd2_proto_id_t)protocol) == ESP_OK) ? 0 : -1;
}
#endif /* !CONFIG_OBD2_VIRTUAL_BENCH */

static elm327_bus_status_t elm_send_obd_req_cb(uint32_t tx_header, uint8_t protocol,
                                               bool extended_mode, const uint8_t *req,
                                               size_t req_len, uint8_t *out_resp, size_t max_resp,
                                               size_t *out_resp_len, uint32_t *out_rx_id,
                                               void *ctx) {
    if (req == NULL || req_len == 0U || out_resp == NULL || out_resp_len == NULL) {
        return ELM_STATUS_NO_DATA;
    }

    /* Enforce default security gating on sensitive services before touching any bus */
    if (!obd2_is_service_allowed(req[0], extended_mode)) {
        if (max_resp >= 3U) {
            out_resp[0] = 0x7FU;
            out_resp[1] = req[0];
            out_resp[2] = 0x33U; /* NRC: SecurityAccessDenied */
            *out_resp_len = 3U;
            if (out_rx_id != NULL) {
                *out_rx_id = 0x7E8U;
            }
            return ELM_STATUS_OK;
        }
        return ELM_STATUS_NO_DATA;
    }

#ifdef CONFIG_OBD2_VIRTUAL_BENCH
    (void)tx_header;
    (void)protocol;
    (void)ctx;

    ecu_simulator_lock();
    int rc = ecu_sim_handle_request(ecu_simulator_state(), extended_mode, req, req_len, out_resp,
                                    max_resp, out_resp_len);
    ecu_simulator_unlock();
    if (rc == 0) {
        if (out_rx_id != NULL) {
            *out_rx_id = 0x7E8U;
        }
        return ELM_STATUS_OK;
    }
    return ELM_STATUS_NO_DATA;
#else
    const elm327_ctx_t *elm = (const elm327_ctx_t *)ctx;

    /* If physical bus ECU simulator mode is running on this board, allow local loopback query too
     */
    if (ecu_simulator_is_bus_running()) {
        ecu_simulator_lock();
        int sim_rc = ecu_sim_handle_request(ecu_simulator_state(), extended_mode, req, req_len,
                                            out_resp, max_resp, out_resp_len);
        ecu_simulator_unlock();
        if (sim_rc == 0) {
            if (out_rx_id != NULL) {
                *out_rx_id = 0x7E8U;
            }
            return ELM_STATUS_OK;
        }
        return ELM_STATUS_NO_DATA;
    }

    obd2_proto_id_t active = (protocol != 0U) ? (obd2_proto_id_t)protocol : s_active_proto;
    if (active == OBD2_PROTO_AUTO) {
        if (obd2_autodetect_run(&active) != ESP_OK) {
            return ELM_STATUS_UNABLE_TO_CONNECT;
        }
        if (activate_physical_protocol(active) != ESP_OK) {
            return ELM_STATUS_UNABLE_TO_CONNECT;
        }
    } else if (s_active_proto != active) {
        if (activate_physical_protocol(active) != ESP_OK) {
            return ELM_STATUS_BUS_INIT_ERROR;
        }
    }

    uint32_t timeout_ms = 200U;
    if (elm != NULL && elm->timeout_st > 0U) {
        timeout_ms = (uint32_t)elm->timeout_st * 4U;
        if (timeout_ms < 50U) {
            timeout_ms = 50U;
        } else if (timeout_ms > 2000U) {
            timeout_ms = 2000U;
        }
    }

    (void)bsp_led_set_pattern(BSP_LED_BURST);

    /* CAN ISO 15765-4 protocols (6..9) */
    if (active >= OBD2_PROTO_CAN_11B_500K && active <= OBD2_PROTO_CAN_29B_250K) {
        bool is_extd = (active == OBD2_PROTO_CAN_29B_500K || active == OBD2_PROTO_CAN_29B_250K);
        uint32_t can_tx_id = tx_header;
        if (is_extd && can_tx_id == 0x7DFU) {
            can_tx_id = 0x18DB33F1UL;
        }
        uint32_t expected_rx_id = is_extd ? 0x18DAF110UL : 0x7E8U;
        if (elm != NULL && elm->rx_filter_id != 0U) {
            expected_rx_id = elm->rx_filter_id;
        }

        (void)twai_drv_check_and_recover(NULL);

        isotp_config_t icfg = {
            .tx_id = can_tx_id,
            .rx_id = expected_rx_id,
            .extd = is_extd,
            .block_size = 0U,
            .stmin_ms = 0U,
            .pad_to_8 = true,
            .padding_byte = 0x55U,
            .n_bs_ms = timeout_ms,
            .n_cr_ms = timeout_ms,
            .send_can = isotp_can_tx_cb,
            .get_ms = isotp_get_ms_cb,
            .user_ctx = NULL,
        };
        (void)isotp_init(&s_isotp_link, &icfg);

        if (isotp_send(&s_isotp_link, req, (uint16_t)req_len) != ISOTP_OK) {
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ELM_STATUS_CAN_ERROR;
        }

        uint32_t start_ms = isotp_get_ms_cb(NULL);
        uint32_t matched_rx_id = expected_rx_id;
        while ((isotp_get_ms_cb(NULL) - start_ms) < timeout_ms) {
            (void)twai_drv_check_and_recover(NULL);
            twai_drv_frame_t rx = {0};
            if (twai_drv_receive(&rx, 10U) == ESP_OK && rx.extd == is_extd && !rx.rtr) {
                bool match = false;
                if (elm != NULL && elm->rx_filter_mask != 0U) {
                    match = ((rx.id & elm->rx_filter_mask) ==
                             (elm->rx_filter_id & elm->rx_filter_mask));
                } else if (!is_extd) {
                    match = (rx.id >= 0x7E8U && rx.id <= 0x7EFU);
                } else {
                    match = ((rx.id & 0xFFFF0000UL) == 0x18DA0000UL);
                }

                if (match) {
                    matched_rx_id = rx.id;
                    s_isotp_link.cfg.rx_id = rx.id;
                    if (elm != NULL && elm->fc_mode != 0U && elm->fc_header != 0U) {
                        s_isotp_link.cfg.tx_id = elm->fc_header;
                    } else if (!is_extd && rx.id >= 0x7E8U && rx.id <= 0x7EFU) {
                        s_isotp_link.cfg.tx_id = rx.id - 8U;
                    }
                    (void)isotp_on_can_rx(&s_isotp_link, rx.id, rx.extd, rx.data, rx.dlc);
                }
            }

            (void)isotp_poll(&s_isotp_link);
            uint16_t got_len = 0U;
            if (isotp_receive(&s_isotp_link, out_resp, (uint16_t)max_resp, &got_len) == ISOTP_OK &&
                got_len > 0U) {
                *out_resp_len = got_len;
                if (out_rx_id != NULL) {
                    *out_rx_id = matched_rx_id;
                }
                (void)bsp_led_set_pattern(BSP_LED_SOLID);
                return ELM_STATUS_OK;
            }
        }

        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ELM_STATUS_NO_DATA;
    }

    /* K-Line protocols (3 = ISO 9141-2, 4 = KWP 5-baud, 5 = KWP Fast) */
    if (active >= OBD2_PROTO_ISO9141_2 && active <= OBD2_PROTO_KWP_FAST) {
        uint8_t target_addr = (tx_header <= 0xFFU) ? (uint8_t)tx_header : 0x33U;
        uint8_t tx_frame[128];
        int tx_len = -1;

        if (active == OBD2_PROTO_ISO9141_2) {
            if (req_len > 7U) {
                (void)bsp_led_set_pattern(BSP_LED_SOLID);
                return ELM_STATUS_NO_DATA;
            }
            tx_len = kline_build_iso9141_frame(0x68U, 0x6AU, 0xF1U, req, (uint8_t)req_len, tx_frame,
                                               sizeof(tx_frame));
        } else {
            uint8_t fmt_base = (target_addr == 0x33U) ? 0xC0U : 0x80U;
            tx_len = kline_build_kwp_frame(fmt_base, target_addr, 0xF1U, req, (uint8_t)req_len,
                                           tx_frame, sizeof(tx_frame));
        }
        if (tx_len <= 0) {
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ELM_STATUS_NO_DATA;
        }

        uint8_t rx_raw[128];
        size_t rx_raw_len = 0U;
        if (kline_drv_send_recv(tx_frame, (size_t)tx_len, rx_raw, sizeof(rx_raw), &rx_raw_len,
                                timeout_ms) != ESP_OK) {
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ELM_STATUS_NO_DATA;
        }

        kline_frame_t kframe;
        if (kline_parse_frame(rx_raw, rx_raw_len, &kframe) != 0 || kframe.payload_len == 0U ||
            kframe.payload_len > max_resp) {
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ELM_STATUS_NO_DATA;
        }

        memcpy(out_resp, kframe.payload, kframe.payload_len);
        *out_resp_len = kframe.payload_len;
        if (out_rx_id != NULL) {
            *out_rx_id = kframe.source;
        }
        (void)bsp_led_set_pattern(BSP_LED_SOLID);
        return ELM_STATUS_OK;
    }

    (void)bsp_led_set_pattern(BSP_LED_SOLID);
    return ELM_STATUS_UNABLE_TO_CONNECT;
#endif
}

static void elm327_host_task(void *arg) {
    (void)arg;
    uint8_t rx_buf[128];

    for (;;) {
        size_t n = transport_stream_read(rx_buf, sizeof(rx_buf), 50U);
        if (n > 0U) {
#ifdef CONFIG_OBD2_VIRTUAL_BENCH
            vbench_log_wire("RX", rx_buf, n);
#endif
            power_mgmt_notify_activity();
            elm327_feed_bytes(&s_elm_ctx, rx_buf, n);
            if (s_elm_ctx.last_cmd[0] != '\0') {
                virtual_bench_ui_set_last_cmd(s_elm_ctx.last_cmd);
            }
        }

#ifndef CONFIG_OBD2_VIRTUAL_BENCH
        if (s_elm_ctx.sniff_enabled && s_active_proto >= OBD2_PROTO_CAN_11B_500K &&
            s_active_proto <= OBD2_PROTO_CAN_29B_250K) {
            (void)twai_drv_check_and_recover(NULL);
            twai_drv_frame_t rx = {0};
            if (twai_drv_receive(&rx, 10U) == ESP_OK && !rx.rtr) {
                bool pass = true;
                if (s_elm_ctx.rx_filter_mask != 0U) {
                    pass = ((rx.id & s_elm_ctx.rx_filter_mask) ==
                            (s_elm_ctx.rx_filter_id & s_elm_ctx.rx_filter_mask));
                }
                if (pass) {
                    char slcan[40];
                    int slen = elm327_format_slcan_frame(rx.id, rx.extd, rx.data, rx.dlc, slcan,
                                                         sizeof(slcan));
                    if (slen > 0) {
                        power_mgmt_notify_activity();
                        (void)transport_stream_write((const uint8_t *)slcan, (size_t)slen);
                    }
                }
            }
        } else if (s_active_proto >= OBD2_PROTO_ISO9141_2 &&
                   s_active_proto <= OBD2_PROTO_KWP_FAST) {
            (void)kline_drv_keepalive_tick(0x33U, 0xF1U);
        }
#endif
    }
}

void app_main(void) {
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

    const esp_app_desc_t *app_desc = esp_app_get_description();
    char fw_ver[64];
#ifdef CONFIG_OBD2_VIRTUAL_BENCH
    snprintf(fw_ver, sizeof(fw_ver), "OBD2-fw %s (VIRTUAL_BENCH)", app_desc->version);
#else
    snprintf(fw_ver, sizeof(fw_ver), "OBD2-fw %s (STD)", app_desc->version);
#endif
    elm327_set_fw_version(fw_ver);

    elm327_register_sim_cmd_cb(elm_sim_cmd_cb);

    elm327_callbacks_t elm_cb = {
        .write_bytes = elm_write_bytes_cb,
        .get_vbat_mv = elm_get_vbat_mv_cb,
        .send_obd_req = elm_send_obd_req_cb,
#ifdef CONFIG_OBD2_VIRTUAL_BENCH
        .set_protocol = NULL,
#else
        .set_protocol = elm_set_protocol_cb,
#endif
        .factory_reset = elm_factory_reset_cb,
        .user_ctx = &s_elm_ctx,
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

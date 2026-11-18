#include "obd2_autodetect.h"

#include "bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "kline_driver.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "twai_driver.h"

static const char *TAG = "obd2_autodetect";

#define NVS_NAMESPACE "obd2"
#define NVS_KEY_PROTO "last_proto"

const char *obd2_proto_name(obd2_proto_id_t proto) {
    switch (proto) {
    case OBD2_PROTO_AUTO:
        return "AUTO";
    case OBD2_PROTO_ISO9141_2:
        return "ISO 9141-2 (5 baud init, 10.4 kbaud)";
    case OBD2_PROTO_KWP_5BAUD:
        return "ISO 14230-4 (KWP 5 baud init, 10.4 kbaud)";
    case OBD2_PROTO_KWP_FAST:
        return "ISO 14230-4 (KWP FAST)";
    case OBD2_PROTO_CAN_11B_500K:
        return "ISO 15765-4 (CAN 11/500)";
    case OBD2_PROTO_CAN_29B_500K:
        return "ISO 15765-4 (CAN 29/500)";
    case OBD2_PROTO_CAN_11B_250K:
        return "ISO 15765-4 (CAN 11/250)";
    case OBD2_PROTO_CAN_29B_250K:
        return "ISO 15765-4 (CAN 29/250)";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t ensure_nvs_ready(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t obd2_nvs_save_proto(obd2_proto_id_t proto) {
    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "nvs init");
    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(h, NVS_KEY_PROTO, (uint8_t)proto);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t obd2_nvs_load_proto(obd2_proto_id_t *out_proto) {
    if (out_proto == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_proto = OBD2_PROTO_AUTO;
    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "nvs init");

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_PROTO, &val);
    nvs_close(h);
    if (err == ESP_OK) {
        *out_proto = (obd2_proto_id_t)val;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static bool probe_can_proto(twai_drv_bitrate_t bitrate, bool extd) {
    if (twai_drv_init(bitrate, TWAI_DRV_MODE_NORMAL, NULL) != ESP_OK) {
        return false;
    }

    twai_drv_frame_t tx = {
        .id = extd ? 0x18DB33F1UL : 0x7DFUL,
        .extd = extd,
        .dlc = 8,
        .data = {0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55},
    };

    bool ok = false;
    if (twai_drv_send(&tx, 100) == ESP_OK) {
        twai_drv_frame_t rx = {0};
        if (twai_drv_receive(&rx, 150) == ESP_OK) {
            ok = true;
        }
    }
    (void)twai_drv_deinit();
    return ok;
}

static bool try_specific_proto(obd2_proto_id_t proto) {
    switch (proto) {
    case OBD2_PROTO_CAN_11B_500K:
        return probe_can_proto(TWAI_DRV_BITRATE_500K, false);
    case OBD2_PROTO_CAN_29B_500K:
        return probe_can_proto(TWAI_DRV_BITRATE_500K, true);
    case OBD2_PROTO_CAN_11B_250K:
        return probe_can_proto(TWAI_DRV_BITRATE_250K, false);
    case OBD2_PROTO_CAN_29B_250K:
        return probe_can_proto(TWAI_DRV_BITRATE_250K, true);
    case OBD2_PROTO_KWP_FAST:
        return (kline_drv_fast_init(0x33U, 0xF1U) == ESP_OK);
    case OBD2_PROTO_KWP_5BAUD: {
        kline_proto_type_t detected = KLINE_PROTO_UNKNOWN;
        return (kline_drv_slow_init(0x33U, &detected, NULL, NULL) == ESP_OK) &&
               (detected == KLINE_PROTO_KWP2000);
    }
    case OBD2_PROTO_ISO9141_2: {
        kline_proto_type_t detected = KLINE_PROTO_UNKNOWN;
        return (kline_drv_slow_init(0x33U, &detected, NULL, NULL) == ESP_OK) &&
               (detected == KLINE_PROTO_ISO9141_2);
    }
    default:
        return false;
    }
}

esp_err_t obd2_autodetect_run(obd2_proto_id_t *out_proto) {
    if (out_proto == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)bsp_led_set_pattern(BSP_LED_BLINK_SLOW);

    obd2_proto_id_t cached = OBD2_PROTO_AUTO;
    (void)obd2_nvs_load_proto(&cached);
    if (cached != OBD2_PROTO_AUTO) {
        ESP_LOGI(TAG, "Trying cached NVS protocol first: %s", obd2_proto_name(cached));
        if (try_specific_proto(cached)) {
            *out_proto = cached;
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ESP_OK;
        }
    }

    /* Passive listen-only CAN bitrate check before active TX */
    twai_drv_bitrate_t passive_rate = TWAI_DRV_BITRATE_500K;
    if (twai_drv_probe_bitrate(150, &passive_rate) == ESP_OK) {
        obd2_proto_id_t c1 = (passive_rate == TWAI_DRV_BITRATE_500K) ? OBD2_PROTO_CAN_11B_500K
                                                                     : OBD2_PROTO_CAN_11B_250K;
        obd2_proto_id_t c2 = (passive_rate == TWAI_DRV_BITRATE_500K) ? OBD2_PROTO_CAN_29B_500K
                                                                     : OBD2_PROTO_CAN_29B_250K;
        if (try_specific_proto(c1)) {
            *out_proto = c1;
            (void)obd2_nvs_save_proto(c1);
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ESP_OK;
        }
        if (try_specific_proto(c2)) {
            *out_proto = c2;
            (void)obd2_nvs_save_proto(c2);
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            return ESP_OK;
        }
    }

    static const obd2_proto_id_t order[7] = {
        OBD2_PROTO_CAN_11B_500K, OBD2_PROTO_CAN_29B_500K, OBD2_PROTO_CAN_11B_250K,
        OBD2_PROTO_CAN_29B_250K, OBD2_PROTO_KWP_FAST,     OBD2_PROTO_KWP_5BAUD,
        OBD2_PROTO_ISO9141_2,
    };

    for (size_t i = 0; i < 7; ++i) {
        if (order[i] == cached) {
            continue;
        }
        if (try_specific_proto(order[i])) {
            *out_proto = order[i];
            (void)obd2_nvs_save_proto(order[i]);
            (void)bsp_led_set_pattern(BSP_LED_SOLID);
            ESP_LOGI(TAG, "ATSP0 locked protocol: %s", obd2_proto_name(order[i]));
            return ESP_OK;
        }
    }

    (void)bsp_led_set_pattern(BSP_LED_BREATHING);
    return ESP_ERR_NOT_FOUND;
}

#include "comm_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "transport_stream.h"

static const char *TAG = "comm_ble";

#define NVS_NAMESPACE "comm_ble"
#define NVS_KEY_NAME "name"
#define NVS_KEY_ENC_REQ "enc_req"
#define BLE_MAX_NAME_LEN 31U

static SemaphoreHandle_t s_ble_mutex = NULL;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_fff1_val_handle = 0;
static uint16_t s_ffe1_val_handle = 0;
static uint16_t s_mtu = 23U;
static bool s_fff1_notify_en = false;
static bool s_ffe1_notify_en = false;
static bool s_require_enc = false;
static char s_device_name[BLE_MAX_NAME_LEN + 1U] = COMM_BLE_DEFAULT_NAME;

static void comm_ble_advertise(void);
static int comm_ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_lock(void) {
    if (s_ble_mutex != NULL) {
        (void)xSemaphoreTake(s_ble_mutex, portMAX_DELAY);
    }
}

static void ble_unlock(void) {
    if (s_ble_mutex != NULL) {
        (void)xSemaphoreGive(s_ble_mutex);
    }
}

static void comm_ble_load_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_device_name);
    err = nvs_get_str(handle, NVS_KEY_NAME, s_device_name, &len);
    if (err != ESP_OK || s_device_name[0] == '\0') {
        strncpy(s_device_name, COMM_BLE_DEFAULT_NAME, sizeof(s_device_name) - 1U);
        s_device_name[sizeof(s_device_name) - 1U] = '\0';
    }
    uint8_t enc = 0U;
    if (nvs_get_u8(handle, NVS_KEY_ENC_REQ, &enc) == ESP_OK) {
        s_require_enc = (enc != 0U);
    }
    nvs_close(handle);
}

static int comm_ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (s_require_enc) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(conn_handle, &desc) != 0 || !desc.sec_state.encrypted) {
                (void)ble_gap_security_initiate(conn_handle);
                return BLE_ATT_ERR_INSUFFICIENT_ENC;
            }
        }

        uint8_t buf[512];
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0U) {
            return 0;
        }
        if (om_len > sizeof(buf)) {
            om_len = sizeof(buf);
        }
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, om_len, &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        esp_err_t err = transport_stream_feed_rx(TRANSPORT_ID_BLE, buf, copied);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE RX dropped (%s)", esp_err_to_name(err));
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_chr_def s_fff0_chrs[] = {
    {
        /* FFF1: TX Notify (Device -> App) */
        .uuid = BLE_UUID16_DECLARE(0xFFF1),
        .access_cb = comm_ble_gatt_access_cb,
        .val_handle = &s_fff1_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        /* FFF2: RX Write / Write No Response (App -> Device) */
        .uuid = BLE_UUID16_DECLARE(0xFFF2),
        .access_cb = comm_ble_gatt_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {0},
};

static struct ble_gatt_chr_def s_ffe0_chrs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0xFFE1),
        .access_cb = comm_ble_gatt_access_cb,
        .val_handle = &s_ffe1_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {0},
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        /* Primary ELM327 BLE UART Service: 0xFFF0 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = s_fff0_chrs,
    },
    {
        /* Alias ELM327 BLE UART Service: 0xFFE0 / 0xFFE1 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFE0),
        .characteristics = s_ffe0_chrs,
    },
    {0},
};

static esp_err_t comm_ble_tx_chunk(uint16_t val_handle, const uint8_t *data, size_t len) {
    uint16_t max_payload = (s_mtu > 3U) ? (uint16_t)(s_mtu - 3U) : 20U;
    size_t offset = 0U;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_payload) {
            chunk = max_payload;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + offset, (uint16_t)chunk);
        if (om == NULL) {
            return ESP_ERR_NO_MEM;
        }
        int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
        if (rc != 0) {
            return ESP_FAIL;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t comm_ble_transport_tx(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_fff1_notify_en) {
        return comm_ble_tx_chunk(s_fff1_val_handle, data, len);
    }
    if (s_ffe1_notify_en) {
        return comm_ble_tx_chunk(s_ffe1_val_handle, data, len);
    }

    /* Fallback if central writes without subscribing CCCD explicitly */
    return comm_ble_tx_chunk(s_fff1_val_handle, data, len);
}

static void comm_ble_advertise(void) {
    char local_name[BLE_MAX_NAME_LEN + 1U];
    ble_lock();
    strncpy(local_name, s_device_name, sizeof(local_name) - 1U);
    local_name[sizeof(local_name) - 1U] = '\0';
    ble_unlock();

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)local_name;
    fields.name_len = (uint8_t)strlen(local_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, comm_ble_gap_event,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static int comm_ble_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_mtu = 23U;
            s_fff1_notify_en = false;
            s_ffe1_notify_en = false;
            ESP_LOGI(TAG, "BLE connected (handle=%u)", s_conn_handle);
            (void)ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            comm_ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_mtu = 23U;
        s_fff1_notify_en = false;
        s_ffe1_notify_en = false;
        transport_stream_release(TRANSPORT_ID_BLE);
        comm_ble_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "BLE MTU updated: %u", s_mtu);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_fff1_val_handle) {
            s_fff1_notify_en = (event->subscribe.cur_notify != 0);
        } else if (event->subscribe.attr_handle == s_ffe1_val_handle) {
            s_ffe1_notify_en = (event->subscribe.cur_notify != 0);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "BLE encryption status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void comm_ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }
    comm_ble_advertise();
}

static void comm_ble_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset (reason=%d)", reason);
}

static void comm_ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t comm_ble_init(void) {
    if (s_ble_mutex == NULL) {
        s_ble_mutex = xSemaphoreCreateMutex();
        if (s_ble_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ble_lock();
    comm_ble_load_config();
    if (s_require_enc) {
        s_fff0_chrs[1].flags |= BLE_GATT_CHR_F_WRITE_ENC;
        s_ffe0_chrs[0].flags |= BLE_GATT_CHR_F_WRITE_ENC;
    }
    ble_unlock();

    esp_err_t err = transport_stream_register_tx(TRANSPORT_ID_BLE, comm_ble_transport_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = comm_ble_on_reset;
    ble_hs_cfg.sync_cb = comm_ble_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = s_require_enc ? 1 : 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        return ESP_FAIL;
    }

    nimble_port_freertos_init(comm_ble_host_task);
    ESP_LOGI(TAG, "comm_ble initialized as '%s' (require_enc=%d)", s_device_name,
             (int)s_require_enc);
    return ESP_OK;
}

esp_err_t comm_ble_set_name(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) > BLE_MAX_NAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_NAME, name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    ble_lock();
    strncpy(s_device_name, name, sizeof(s_device_name) - 1U);
    s_device_name[sizeof(s_device_name) - 1U] = '\0';
    (void)ble_svc_gap_device_name_set(s_device_name);
    ble_unlock();
    return ESP_OK;
}

esp_err_t comm_ble_set_require_encryption(bool require_enc) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_ENC_REQ, require_enc ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ble_lock();
        s_require_enc = require_enc;
        ble_unlock();
    }
    return err;
}

bool comm_ble_is_connected(void) {
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

uint16_t comm_ble_get_mtu(void) {
    return s_mtu;
}

#include "transport_stream.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#define TRANSPORT_RX_BUF_BYTES 2048U

static StaticRingbuffer_t s_rb_struct;
static uint8_t s_rb_storage[TRANSPORT_RX_BUF_BYTES];
static RingbufHandle_t s_rb = NULL;
static SemaphoreHandle_t s_lock = NULL;

static transport_id_t s_active_id = TRANSPORT_ID_NONE;
static int64_t s_last_rx_us = 0;
static uint32_t s_lock_timeout_ms = 30000U;

typedef struct {
    transport_tx_fn_t fn;
    void *ctx;
} tx_entry_t;

static tx_entry_t s_tx_table[4] = {0};

esp_err_t transport_stream_init(uint32_t lock_timeout_ms) {
    if (s_rb == NULL) {
        s_rb = xRingbufferCreateStatic(TRANSPORT_RX_BUF_BYTES, RINGBUF_TYPE_BYTEBUF, s_rb_storage,
                                       &s_rb_struct);
        if (s_rb == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_lock_timeout_ms = (lock_timeout_ms > 0U) ? lock_timeout_ms : 30000U;
    s_active_id = TRANSPORT_ID_NONE;
    return ESP_OK;
}

esp_err_t transport_stream_register_tx(transport_id_t id, transport_tx_fn_t tx_fn, void *ctx) {
    if (id <= TRANSPORT_ID_NONE || id > TRANSPORT_ID_WIFI) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tx_table[id].fn = tx_fn;
    s_tx_table[id].ctx = ctx;
    return ESP_OK;
}

esp_err_t transport_stream_feed_rx(transport_id_t id, const uint8_t *data, size_t len) {
    if (s_rb == NULL || s_lock == NULL || data == NULL || len == 0U || id <= TRANSPORT_ID_NONE ||
        id > TRANSPORT_ID_WIFI) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool expired = ((now_us - s_last_rx_us) / 1000LL) >= (int64_t)s_lock_timeout_ms;
    if (s_active_id == TRANSPORT_ID_NONE || expired) {
        s_active_id = id;
    }
    if (s_active_id != id) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_last_rx_us = now_us;
    xSemaphoreGive(s_lock);

    BaseType_t ok = xRingbufferSend(s_rb, data, len, pdMS_TO_TICKS(50));
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}

void transport_stream_release(transport_id_t id) {
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_active_id == id) {
            s_active_id = TRANSPORT_ID_NONE;
        }
        xSemaphoreGive(s_lock);
    }
}

size_t transport_stream_read(uint8_t *out_buf, size_t max_len, uint32_t timeout_ms) {
    if (s_rb == NULL || out_buf == NULL || max_len == 0U) {
        return 0U;
    }
    size_t item_size = 0U;
    uint8_t *item =
        (uint8_t *)xRingbufferReceiveUpTo(s_rb, &item_size, pdMS_TO_TICKS(timeout_ms), max_len);
    if (item == NULL || item_size == 0U) {
        return 0U;
    }
    memcpy(out_buf, item, item_size);
    vRingbufferReturnItem(s_rb, item);
    return item_size;
}

esp_err_t transport_stream_write(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0U || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    transport_id_t active = TRANSPORT_ID_NONE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        active = s_active_id;
        xSemaphoreGive(s_lock);
    }
    if (active <= TRANSPORT_ID_NONE || active > TRANSPORT_ID_WIFI ||
        s_tx_table[active].fn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = s_tx_table[active].fn(data, len, s_tx_table[active].ctx);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

transport_id_t transport_stream_active_id(void) {
    return s_active_id;
}

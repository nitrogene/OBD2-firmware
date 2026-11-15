#include "kline_driver.h"

#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kline_driver";

#define KLINE_UART_PORT UART_NUM_1

static bool s_uart_installed = false;
static kline_timing_cfg_t s_timings = {
    .p1_max_ms = 20,
    .p2_max_ms = 50,
    .p3_min_ms = 55,
    .p4_min_ms = 5,
    .keepalive_ms = 2000,
};
static int64_t s_last_tx_us = 0;

static esp_err_t install_uart(void)
{
    if (s_uart_installed) {
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = KLINE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(KLINE_UART_PORT, 512, 0, 0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(KLINE_UART_PORT, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(KLINE_UART_PORT, BSP_PIN_KLINE_TX, BSP_PIN_KLINE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
        TAG,
        "uart_set_pin");

    s_uart_installed = true;
    return ESP_OK;
}

esp_err_t kline_drv_init(const kline_timing_cfg_t *timings)
{
    if (timings != NULL) {
        s_timings = *timings;
    }
    ESP_RETURN_ON_ERROR(install_uart(), TAG, "install_uart failed");
    s_last_tx_us = esp_timer_get_time();
    ESP_LOGI(TAG, "K-Line UART1 initialized at %d baud (P3=%u ms, P4=%u ms)",
             KLINE_BAUD_RATE, s_timings.p3_min_ms, s_timings.p4_min_ms);
    return ESP_OK;
}

esp_err_t kline_drv_deinit(void)
{
    if (!s_uart_installed) {
        return ESP_OK;
    }
    esp_err_t err = uart_driver_delete(KLINE_UART_PORT);
    s_uart_installed = false;
    return err;
}

static void enforce_p3_gap(void)
{
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_tx_us) / 1000LL;
    if (elapsed_ms >= 0 && elapsed_ms < (int64_t)s_timings.p3_min_ms) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(s_timings.p3_min_ms - elapsed_ms)));
    }
}

static esp_err_t send_with_echo_check(const uint8_t *tx_frame, size_t tx_len)
{
    (void)uart_flush_input(KLINE_UART_PORT);

    for (size_t i = 0; i < tx_len; ++i) {
        int written = uart_write_bytes(KLINE_UART_PORT, &tx_frame[i], 1);
        if (written != 1) {
            return ESP_FAIL;
        }
        (void)uart_wait_tx_done(KLINE_UART_PORT, pdMS_TO_TICKS(50));

        uint8_t echo = 0;
        int got = uart_read_bytes(KLINE_UART_PORT, &echo, 1, pdMS_TO_TICKS(30));
        if (got != 1 || !kline_verify_echo(&tx_frame[i], &echo, 1)) {
            ESP_LOGW(TAG, "K-Line echo mismatch/collision at byte %u (tx=0x%02X rx=0x%02X got=%d)",
                     (unsigned)i, tx_frame[i], echo, got);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (s_timings.p4_min_ms > 0U && i + 1U < tx_len) {
            esp_rom_delay_us((uint32_t)s_timings.p4_min_ms * 1000U);
        }
    }

    s_last_tx_us = esp_timer_get_time();
    return ESP_OK;
}

esp_err_t kline_drv_send_recv(const uint8_t *tx_frame,
                              size_t tx_len,
                              uint8_t *rx_buf,
                              size_t max_rx,
                              size_t *out_rx_len,
                              uint32_t timeout_ms)
{
    if (!s_uart_installed || tx_frame == NULL || tx_len == 0U || rx_buf == NULL || out_rx_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    enforce_p3_gap();
    ESP_RETURN_ON_ERROR(send_with_echo_check(tx_frame, tx_len), TAG, "TX echo check failed");

    size_t total = 0U;
    uint32_t wait_ms = (timeout_ms > 0U) ? timeout_ms : s_timings.p2_max_ms;

    while (total < max_rx) {
        uint8_t byte = 0;
        int got = uart_read_bytes(KLINE_UART_PORT, &byte, 1, pdMS_TO_TICKS(wait_ms));
        if (got <= 0) {
            break;
        }
        rx_buf[total++] = byte;
        /* Subsequent bytes must arrive within P1_max */
        wait_ms = s_timings.p1_max_ms;
    }

    *out_rx_len = total;
    s_last_tx_us = esp_timer_get_time();
    return (total > 0U) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void bitbang_5baud_byte(uint8_t addr)
{
    /* 200 ms per bit: Start (0), 8 data bits LSB-first, Stop (1) */
    (void)gpio_set_level(BSP_PIN_KLINE_TX, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    for (int bit = 0; bit < 8; ++bit) {
        (void)gpio_set_level(BSP_PIN_KLINE_TX, (addr >> bit) & 1U);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

esp_err_t kline_drv_slow_init(uint8_t target_addr,
                              kline_proto_type_t *out_proto,
                              uint8_t *out_kb1,
                              uint8_t *out_kb2)
{
    (void)kline_drv_deinit();

    const gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_KLINE_TX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tx_cfg), TAG, "gpio_config slow init");

    /* Bus idle HIGH >= 2600 ms before 5-baud address */
    (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
    vTaskDelay(pdMS_TO_TICKS(2600));

    bitbang_5baud_byte(target_addr);
    ESP_RETURN_ON_ERROR(install_uart(), TAG, "install_uart after 5-baud");
    (void)uart_flush_input(KLINE_UART_PORT);

    uint8_t sync_and_kb[3] = {0};
    int got = uart_read_bytes(KLINE_UART_PORT, sync_and_kb, 3, pdMS_TO_TICKS(500));
    if (got != 3 || sync_and_kb[0] != 0x55U) {
        ESP_LOGW(TAG, "5-baud sync failed (got=%d, byte0=0x%02X)", got, sync_and_kb[0]);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t kb1 = sync_and_kb[1];
    uint8_t kb2 = sync_and_kb[2];
    if (out_kb1 != NULL) {
        *out_kb1 = kb1;
    }
    if (out_kb2 != NULL) {
        *out_kb2 = kb2;
    }
    if (out_proto != NULL) {
        *out_proto = kline_classify_keybytes(kb1, kb2);
    }

    /* Send inverted ~KB2 and expect inverted ~target_addr */
    vTaskDelay(pdMS_TO_TICKS(25));
    uint8_t inv_kb2 = (uint8_t)(~kb2);
    ESP_RETURN_ON_ERROR(send_with_echo_check(&inv_kb2, 1), TAG, "~KB2 echo failed");

    uint8_t inv_addr = 0;
    got = uart_read_bytes(KLINE_UART_PORT, &inv_addr, 1, pdMS_TO_TICKS(100));
    if (got != 1 || inv_addr != (uint8_t)(~target_addr)) {
        ESP_LOGW(TAG, "5-baud ~addr mismatch (got=0x%02X expected=0x%02X)",
                 inv_addr, (uint8_t)(~target_addr));
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_last_tx_us = esp_timer_get_time();
    ESP_LOGI(TAG, "5-baud Slow Init OK (KB1=0x%02X KB2=0x%02X)", kb1, kb2);
    return ESP_OK;
}

esp_err_t kline_drv_fast_init(uint8_t target_addr, uint8_t source_addr)
{
    (void)kline_drv_deinit();

    const gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << BSP_PIN_KLINE_TX),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tx_cfg), TAG, "gpio_config fast init");

    /* Bus idle HIGH >= 300 ms, then 25 ms dominant LOW + 25 ms recessive HIGH */
    (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    (void)gpio_set_level(BSP_PIN_KLINE_TX, 0);
    esp_rom_delay_us(25000);
    (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
    esp_rom_delay_us(25000);

    ESP_RETURN_ON_ERROR(install_uart(), TAG, "install_uart after fast init");

    const uint8_t start_comm = 0x81U;
    uint8_t tx[8];
    int tx_len = kline_build_kwp_frame(0x80U, target_addr, source_addr, &start_comm, 1, tx, sizeof(tx));
    if (tx_len < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rx[16] = {0};
    size_t rx_len = 0U;
    ESP_RETURN_ON_ERROR(kline_drv_send_recv(tx, (size_t)tx_len, rx, sizeof(rx), &rx_len, 150),
                        TAG, "StartCommunication failed");

    kline_frame_t frame;
    if (kline_parse_frame(rx, rx_len, &frame) != 0 || frame.payload_len < 1U || frame.payload[0] != 0xC1U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Fast-Init OK (target=0x%02X)", target_addr);
    return ESP_OK;
}

esp_err_t kline_drv_keepalive_tick(uint8_t target_addr, uint8_t source_addr)
{
    if (!s_uart_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    int64_t idle_ms = (esp_timer_get_time() - s_last_tx_us) / 1000LL;
    if (idle_ms < (int64_t)s_timings.keepalive_ms) {
        return ESP_OK;
    }

    const uint8_t tp = 0x3EU; /* TesterPresent */
    uint8_t tx[8];
    int tx_len = kline_build_kwp_frame(0x80U, target_addr, source_addr, &tp, 1, tx, sizeof(tx));
    if (tx_len < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rx[16] = {0};
    size_t rx_len = 0U;
    return kline_drv_send_recv(tx, (size_t)tx_len, rx, sizeof(rx), &rx_len, 100);
}

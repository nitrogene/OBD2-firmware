#include "power_mgmt.h"

#include "bsp.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "power_mgmt";

#define POWER_MA_WINDOW 32U
#define POWER_SAMPLE_PERIOD_MS 250U

static SemaphoreHandle_t s_mutex = NULL;
static power_status_t s_status = {0};
static int64_t s_last_activity_us = 0;

static uint32_t s_vbat_ring[POWER_MA_WINDOW] = {0};
static uint32_t s_adc_ring[POWER_MA_WINDOW] = {0};
static uint32_t s_ring_idx = 0;
static uint32_t s_ring_count = 0;
static uint64_t s_vbat_sum = 0;
static uint64_t s_adc_sum = 0;

power_state_t power_classify_vbat_mv(uint32_t vbat_mv) {
    if (vbat_mv < POWER_THRESH_USB_ONLY_MV) {
        return POWER_STATE_USB_ONLY;
    }
    if (vbat_mv < POWER_THRESH_CRITICAL_MV) {
        return POWER_STATE_UNDERVOLTAGE_CRITICAL;
    }
    if (vbat_mv < POWER_THRESH_ENGINE_ON_MV) {
        return POWER_STATE_RESTING;
    }
    if (vbat_mv <= POWER_THRESH_OVERVOLTAGE_MV) {
        return POWER_STATE_ENGINE_RUNNING;
    }
    return POWER_STATE_OVERVOLTAGE;
}

const char *power_mgmt_state_to_str(power_state_t state) {
    switch (state) {
    case POWER_STATE_USB_ONLY:
        return "USB_ONLY";
    case POWER_STATE_UNDERVOLTAGE_CRITICAL:
        return "CRITICAL_LOW";
    case POWER_STATE_RESTING:
        return "RESTING";
    case POWER_STATE_ENGINE_RUNNING:
        return "ENGINE_RUNNING";
    case POWER_STATE_OVERVOLTAGE:
        return "OVERVOLTAGE";
    default:
        return "UNKNOWN";
    }
}

static void update_moving_average(uint32_t adc_mv, uint32_t vbat_mv, uint32_t *out_avg_adc,
                                  uint32_t *out_avg_vbat) {
    if (s_ring_count < POWER_MA_WINDOW) {
        s_ring_count++;
    } else {
        s_adc_sum -= s_adc_ring[s_ring_idx];
        s_vbat_sum -= s_vbat_ring[s_ring_idx];
    }

    s_adc_ring[s_ring_idx] = adc_mv;
    s_vbat_ring[s_ring_idx] = vbat_mv;
    s_adc_sum += adc_mv;
    s_vbat_sum += vbat_mv;
    s_ring_idx = (s_ring_idx + 1U) % POWER_MA_WINDOW;

    *out_avg_adc = (uint32_t)(s_adc_sum / s_ring_count);
    *out_avg_vbat = (uint32_t)(s_vbat_sum / s_ring_count);
}

static void power_telemetry_task(void *arg) {
    (void)arg;
    power_state_t prev_state = POWER_STATE_USB_ONLY;
    bool first_sample = true;

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "TWDT add failed (%s)", esp_err_to_name(wdt_err));
    }

    while (1) {
        (void)esp_task_wdt_reset();
        uint32_t raw_adc_mv = 0;
        uint32_t raw_vbat_mv = 0;

        if (bsp_vbat_read_mv(&raw_adc_mv, &raw_vbat_mv) == ESP_OK) {
            uint32_t avg_adc_mv = 0;
            uint32_t avg_vbat_mv = 0;
            update_moving_average(raw_adc_mv, raw_vbat_mv, &avg_adc_mv, &avg_vbat_mv);

            power_state_t new_state = power_classify_vbat_mv(avg_vbat_mv);
            int64_t now_us = esp_timer_get_time();
            uint32_t inactivity_ms = 0U;
            bool sleep_armed = (new_state == POWER_STATE_RESTING) ||
                               (new_state == POWER_STATE_UNDERVOLTAGE_CRITICAL);

            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (new_state == POWER_STATE_ENGINE_RUNNING) {
                    s_last_activity_us = now_us;
                }
                inactivity_ms = (uint32_t)((now_us - s_last_activity_us) / 1000LL);
                s_status.adc_mv = avg_adc_mv;
                s_status.vbat_mv = avg_vbat_mv;
                s_status.state = new_state;
                s_status.inactivity_ms = inactivity_ms;
                s_status.auto_sleep_armed = sleep_armed;
                xSemaphoreGive(s_mutex);
            }

            if (first_sample || new_state != prev_state) {
                ESP_LOGI(TAG, "Power state -> %s (V_BAT=%lu mV, V_ADC=%lu mV)",
                         power_mgmt_state_to_str(new_state), (unsigned long)avg_vbat_mv,
                         (unsigned long)avg_adc_mv);
                if (new_state == POWER_STATE_OVERVOLTAGE) {
                    ESP_LOGE(TAG, "OVERVOLTAGE FAULT (> 15.5 V)!");
                    bsp_led_set_pattern(BSP_LED_SOS);
                } else if (!first_sample && prev_state == POWER_STATE_OVERVOLTAGE) {
                    bsp_led_set_pattern(BSP_LED_BREATHING);
                } else if (new_state == POWER_STATE_UNDERVOLTAGE_CRITICAL) {
                    ESP_LOGW(TAG, "Critically discharged battery (< 11.8 V)");
                }
                prev_state = new_state;
                first_sample = false;
            }

#ifndef CONFIG_OBD2_VIRTUAL_BENCH
            if (sleep_armed && inactivity_ms >= POWER_INACTIVITY_TIMEOUT_MS) {
                ESP_LOGW(TAG, "10-minute inactivity timeout in %s; entering Deep Sleep",
                         power_mgmt_state_to_str(new_state));
                power_mgmt_enter_deep_sleep();
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_SAMPLE_PERIOD_MS));
    }
}

esp_err_t power_mgmt_boot_self_test(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
        ESP_LOGW(TAG, "Previous reset was triggered by hardware watchdog (reason=%d)", (int)reason);
    } else {
        ESP_LOGI(TAG, "Boot reset reason=%d", (int)reason);
    }

    uint32_t adc_mv = 0;
    uint32_t vbat_mv = 0;
    esp_err_t err = bsp_vbat_read_mv(&adc_mv, &vbat_mv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot self-test VBAT_SENSE failed: %s", esp_err_to_name(err));
        return err;
    }

    power_state_t st = power_classify_vbat_mv(vbat_mv);
    ESP_LOGI(TAG, "Boot self-test: V_ADC=%lu mV, V_BAT=%lu mV (%s)", (unsigned long)adc_mv,
             (unsigned long)vbat_mv, power_mgmt_state_to_str(st));
    return ESP_OK;
}

esp_err_t power_mgmt_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_status.wakeup_cause = esp_sleep_get_wakeup_cause();
#ifndef CONFIG_OBD2_VIRTUAL_BENCH
    if (s_status.wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
        uint32_t adc_mv = 0;
        uint32_t vbat_mv = 0;
        if (bsp_vbat_read_mv(&adc_mv, &vbat_mv) == ESP_OK && vbat_mv < POWER_THRESH_ENGINE_ON_MV) {
            /* Engine still off (< 13.2 V); return immediately to Deep Sleep */
            power_mgmt_enter_deep_sleep();
        }
        ESP_LOGI(TAG, "Woke up on VBAT_SENSE > 13.2 V (%lu mV)", (unsigned long)vbat_mv);
    } else if (s_status.wakeup_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "Woke up from Deep Sleep (cause=%d)", (int)s_status.wakeup_cause);
    }
#endif

    const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 5000,
        .idle_core_mask = (1U << 0) | (1U << 1),
        .trigger_panic = true,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (wdt_err == ESP_ERR_INVALID_STATE) {
        wdt_err = esp_task_wdt_init(&wdt_cfg);
    }
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "TWDT config skipped (%s)", esp_err_to_name(wdt_err));
    } else {
        ESP_LOGI(TAG, "Hardware TWDT active (5000 ms timeout, panic on stall)");
    }

    s_last_activity_us = esp_timer_get_time();
    (void)power_mgmt_boot_self_test();

    BaseType_t ok = xTaskCreate(power_telemetry_task, "power_telemetry", 3072, NULL, 3, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "power_telemetry task started (10-min auto-sleep armed on resting 12V)");
    return ESP_OK;
}

esp_err_t power_mgmt_get_status(power_status_t *out_status) {
    if (out_status == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_status = s_status;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void power_mgmt_notify_activity(void) {
    int64_t now_us = esp_timer_get_time();
    if (s_mutex != NULL) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_last_activity_us = now_us;
            xSemaphoreGive(s_mutex);
        }
    } else {
        s_last_activity_us = now_us;
    }
}

void __attribute__((noreturn)) power_mgmt_enter_deep_sleep(void) {
    bsp_led_set_pattern(BSP_LED_OFF);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Enable ext1 wake-up on dominant LOW edge from KLINE_RX (IO4) or TWAI_RX (IO16) */
    const uint64_t wakeup_mask = (1ULL << BSP_PIN_KLINE_RX) | (1ULL << BSP_PIN_TWAI_RX);
    (void)rtc_gpio_init(BSP_PIN_KLINE_RX);
    (void)rtc_gpio_set_direction(BSP_PIN_KLINE_RX, RTC_GPIO_MODE_INPUT_ONLY);
    (void)rtc_gpio_pullup_en(BSP_PIN_KLINE_RX);
    (void)rtc_gpio_pulldown_dis(BSP_PIN_KLINE_RX);

    (void)rtc_gpio_init(BSP_PIN_TWAI_RX);
    (void)rtc_gpio_set_direction(BSP_PIN_TWAI_RX, RTC_GPIO_MODE_INPUT_ONLY);
    (void)rtc_gpio_pullup_en(BSP_PIN_TWAI_RX);
    (void)rtc_gpio_pulldown_dis(BSP_PIN_TWAI_RX);

    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW));
    /* Also enable periodic RTC timer wake-up to sample VBAT_SENSE (> 13.2 V alternator crank on
     * K-Line vehicles) */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL));
    ESP_LOGI(TAG, "Entering Deep Sleep (ext1 wake-up on IO%d / IO%d LOW + 10s VBAT > 13.2V check)",
             (int)BSP_PIN_KLINE_RX, (int)BSP_PIN_TWAI_RX);

    esp_deep_sleep_start();
}

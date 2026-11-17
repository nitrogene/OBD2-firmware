#include "bsp.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_led";

#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LEDC_MAX_DUTY ((1U << 10) - 1U)
#define LEDC_FREQ_HZ 5000

static volatile bsp_led_pattern_t s_pattern = BSP_LED_OFF;
static volatile uint8_t s_manual_percent = 0;

static void apply_duty_percent(uint8_t percent) {
    if (percent > 100U) {
        percent = 100U;
    }
    uint32_t duty = (LEDC_MAX_DUTY * percent) / 100U;
    (void)ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    (void)ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void run_sos_pattern(void) {
    /* 3 short (150 ms), 3 long (450 ms), 3 short (150 ms), then pause */
    static const uint16_t on_ms[9] = {150, 150, 150, 450, 450, 450, 150, 150, 150};

    for (size_t i = 0; i < 9 && s_pattern == BSP_LED_SOS; ++i) {
        apply_duty_percent(100);
        vTaskDelay(pdMS_TO_TICKS(on_ms[i]));
        apply_duty_percent(0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (s_pattern == BSP_LED_SOS) {
        vTaskDelay(pdMS_TO_TICKS(600));
    }
}

static void ui_led_task(void *arg) {
    (void)arg;
    uint32_t tick_step = 0;

    while (1) {
        bsp_led_pattern_t pattern = s_pattern;
        switch (pattern) {
        case BSP_LED_OFF:
            apply_duty_percent(0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case BSP_LED_SOLID:
            apply_duty_percent(100);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case BSP_LED_MANUAL:
            apply_duty_percent(s_manual_percent);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case BSP_LED_BREATHING: {
            /* 0.5 Hz triangle ramp: 20 ms step * 100 steps = 2000 ms period */
            uint32_t phase = tick_step % 100U;
            uint8_t pct = (phase < 50U) ? (uint8_t)(phase * 2U) : (uint8_t)((100U - phase) * 2U);
            apply_duty_percent(pct);
            tick_step++;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case BSP_LED_BLINK_SLOW:
            /* 1 Hz: 500 ms ON / 500 ms OFF */
            apply_duty_percent(100);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_pattern != BSP_LED_BLINK_SLOW) {
                break;
            }
            apply_duty_percent(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case BSP_LED_BURST:
            /* 10 Hz: 50 ms ON / 50 ms OFF */
            apply_duty_percent(100);
            vTaskDelay(pdMS_TO_TICKS(50));
            apply_duty_percent(0);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case BSP_LED_SOS:
            run_sos_pattern();
            break;

        default:
            apply_duty_percent(0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

esp_err_t bsp_led_init(void) {
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer config failed");

    const ledc_channel_config_t ch_cfg = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BSP_PIN_LED_STATUS,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc channel config failed");

    BaseType_t ok = xTaskCreate(ui_led_task, "ui_led", 2048, NULL, 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ui_led task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED1 PWM initialized on IO%d", (int)BSP_PIN_LED_STATUS);
    return ESP_OK;
}

void bsp_led_set_pattern(bsp_led_pattern_t pattern) {
    s_pattern = pattern;
}

void bsp_led_set_duty(uint8_t duty_percent) {
    s_manual_percent = (duty_percent > 100U) ? 100U : duty_percent;
    s_pattern = BSP_LED_MANUAL;
}

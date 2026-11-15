#include "bsp.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp";

#define BSP_VBAT_SAMPLES 32

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_enabled = false;

static esp_err_t bsp_adc_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BSP_VBAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle), TAG, "adc_oneshot_new_unit failed");

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BSP_VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(s_adc_handle, BSP_VBAT_ADC_CH, &chan_cfg),
        TAG,
        "adc_oneshot_config_channel failed");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BSP_VBAT_ADC_UNIT,
        .chan = BSP_VBAT_ADC_CH,
        .atten = BSP_VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t cali_err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (cali_err == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "ADC1_CH0 curve-fitting calibration enabled (IO%d)", (int)BSP_PIN_VBAT_SENSE);
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s), using raw linear estimate", esp_err_to_name(cali_err));
    }
#endif

    return ESP_OK;
}

esp_err_t bsp_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_led_init(), TAG, "bsp_led_init failed");
    ESP_RETURN_ON_ERROR(bsp_adc_init(), TAG, "bsp_adc_init failed");

    ESP_LOGI(TAG,
             "BSP ready: VBAT=IO%d LED=IO%d KLINE_RX=IO%d KLINE_TX=IO%d TWAI_TX=IO%d TWAI_RX=IO%d",
             (int)BSP_PIN_VBAT_SENSE,
             (int)BSP_PIN_LED_STATUS,
             (int)BSP_PIN_KLINE_RX,
             (int)BSP_PIN_KLINE_TX,
             (int)BSP_PIN_TWAI_TX,
             (int)BSP_PIN_TWAI_RX);
    return ESP_OK;
}

esp_err_t bsp_vbat_read_mv(uint32_t *out_adc_mv, uint32_t *out_vbat_mv)
{
    if (s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t sum_raw = 0;
    for (int i = 0; i < BSP_VBAT_SAMPLES; ++i) {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, BSP_VBAT_ADC_CH, &raw), TAG, "adc_oneshot_read failed");
        sum_raw += raw;
    }
    int avg_raw = (int)(sum_raw / BSP_VBAT_SAMPLES);

    int adc_mv = 0;
    if (s_cali_enabled) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, avg_raw, &adc_mv), TAG, "adc_cali_raw_to_voltage failed");
    } else {
        /* Fallback linear approximation for 12-bit ADC at ~3100 mV full scale */
        adc_mv = (avg_raw * 3100) / 4095;
    }

    if (adc_mv < 0) {
        adc_mv = 0;
    }

    uint32_t vbat_mv = (uint32_t)((float)adc_mv * BSP_VBAT_INV_RATIO + 0.5f);

    if (out_adc_mv != NULL) {
        *out_adc_mv = (uint32_t)adc_mv;
    }
    if (out_vbat_mv != NULL) {
        *out_vbat_mv = vbat_mv;
    }

    return ESP_OK;
}

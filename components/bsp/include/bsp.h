#ifndef BSP_H
#define BSP_H

#include <stdint.h>
#include "esp_err.h"
#include "bsp_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status LED1 (IO2) state machine patterns.
 */
typedef enum {
    BSP_LED_OFF = 0,    /**< 0%: Unpowered or Deep Sleep */
    BSP_LED_BREATHING,  /**< 0.5 Hz PWM ramp: Ready / idle */
    BSP_LED_BLINK_SLOW, /**< 1 Hz (500 ms ON / 500 ms OFF): Scanning / init */
    BSP_LED_SOLID,      /**< 100%: Vehicle bus connected and active */
    BSP_LED_BURST,      /**< 10 Hz (50 ms bursts): Active frame TX/RX */
    BSP_LED_SOS,        /**< SOS Morse: Critical fault (overvoltage / Bus-Off) */
    BSP_LED_MANUAL      /**< Manual PWM duty override */
} bsp_led_pattern_t;

/**
 * @brief Initialize Board Support Package (LED1 PWM + VBAT_SENSE ADC1_CH0).
 *
 * @return ESP_OK on success, or an ESP-IDF error code.
 */
esp_err_t bsp_init(void);

/**
 * @brief Initialize LED1 PWM driver and ui_led background task.
 *
 * @return ESP_OK on success.
 */
esp_err_t bsp_led_init(void);

/**
 * @brief Set the active LED1 pattern.
 *
 * @param pattern Desired LED pattern.
 */
void bsp_led_set_pattern(bsp_led_pattern_t pattern);

/**
 * @brief Set a manual LED1 PWM duty cycle (0-100%).
 *
 * Automatically switches LED1 mode to BSP_LED_MANUAL.
 *
 * @param duty_percent Duty cycle percentage (0..100).
 */
void bsp_led_set_duty(uint8_t duty_percent);

/**
 * @brief Sample VBAT_SENSE (IO1 / ADC1_CH0) with 32-sample averaging.
 *
 * @param[out] out_adc_mv   Optional calibrated ADC pin voltage in mV.
 * @param[out] out_vbat_mv  Optional scaled vehicle battery voltage in mV.
 * @return ESP_OK on success.
 */
esp_err_t bsp_vbat_read_mv(uint32_t *out_adc_mv, uint32_t *out_vbat_mv);

/**
 * @brief Override simulated V_BAT voltage in mV (used when CONFIG_OBD2_VIRTUAL_BENCH is enabled).
 */
void bsp_vbat_set_sim_mv(uint32_t vbat_mv);

#ifdef __cplusplus
}
#endif

#endif /* BSP_H */

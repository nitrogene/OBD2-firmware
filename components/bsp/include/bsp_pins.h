#ifndef BSP_PINS_H
#define BSP_PINS_H

#include "driver/gpio.h"
#include "hal/adc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware GPIO Mapping (Single Source of Truth)
 * Reference: nitrogene/OBD2 documents/software.md & README.md
 */
#define BSP_PIN_BOOT         GPIO_NUM_0   /* IO0: Strapping / ROM Bootloader (TP10) */
#define BSP_PIN_VBAT_SENSE   GPIO_NUM_1   /* IO1: ADC1_CH0 Battery Voltage Divider */
#define BSP_PIN_LED_STATUS   GPIO_NUM_2   /* IO2: Status LED1 (Active HIGH, PWM) */
#define BSP_PIN_KLINE_RX     GPIO_NUM_4   /* IO4: UART RX from U3 (L9637D) */
#define BSP_PIN_KLINE_TX     GPIO_NUM_5   /* IO5: UART TX to U3 (L9637D) */
#define BSP_PIN_TWAI_TX      GPIO_NUM_15  /* IO15: TWAI TX to U2 (TJA1051T) */
#define BSP_PIN_TWAI_RX      GPIO_NUM_16  /* IO16: TWAI RX from U2 (TJA1051T) */
#define BSP_PIN_USB_DM       GPIO_NUM_19  /* IO19: Native USB OTG D- */
#define BSP_PIN_USB_DP       GPIO_NUM_20  /* IO20: Native USB OTG D+ */

/* VBAT_SENSE Analog Front-End: R12 = 100 kOhm, R13 = 12 kOhm */
#define BSP_VBAT_ADC_UNIT    ADC_UNIT_1
#define BSP_VBAT_ADC_CH      ADC_CHANNEL_0
#define BSP_VBAT_ADC_ATTEN   ADC_ATTEN_DB_12
#define BSP_VBAT_DIV_NUM     12.0f
#define BSP_VBAT_DIV_DEN     112.0f
#define BSP_VBAT_INV_RATIO   (BSP_VBAT_DIV_DEN / BSP_VBAT_DIV_NUM) /* 9.3333f */

#ifdef __cplusplus
}
#endif

#endif /* BSP_PINS_H */

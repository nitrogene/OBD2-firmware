#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Voltage thresholds in millivolts (per README.md Section 4) */
#define POWER_THRESH_USB_ONLY_MV       2800U
#define POWER_THRESH_CRITICAL_MV       11800U
#define POWER_THRESH_ENGINE_ON_MV      13200U
#define POWER_THRESH_OVERVOLTAGE_MV    15500U

/* 10-minute inactivity timeout before Deep Sleep */
#define POWER_INACTIVITY_TIMEOUT_MS    (10UL * 60UL * 1000UL)

/**
 * @brief Vehicle power state derived from VBAT_SENSE (IO1 / ADC1_CH0).
 */
typedef enum {
    POWER_STATE_USB_ONLY = 0,          /**< < 2.8 V: USB bench debug, +12V_PROT isolated */
    POWER_STATE_UNDERVOLTAGE_CRITICAL, /**< 2.8 V - 11.799 V: Critically discharged */
    POWER_STATE_RESTING,               /**< 11.8 V - 13.199 V: Engine off / resting battery */
    POWER_STATE_ENGINE_RUNNING,        /**< 13.2 V - 15.500 V: Alternator active */
    POWER_STATE_OVERVOLTAGE            /**< > 15.5 V: Overvoltage fault */
} power_state_t;

/**
 * @brief Snapshot of power telemetry and sleep governance state.
 */
typedef struct {
    uint32_t adc_mv;
    uint32_t vbat_mv;
    power_state_t state;
    uint32_t inactivity_ms;
    bool auto_sleep_armed;
    esp_sleep_wakeup_cause_t wakeup_cause;
} power_status_t;

/**
 * @brief Classify vehicle battery voltage (in mV) into a power_state_t.
 */
power_state_t power_classify_vbat_mv(uint32_t vbat_mv);

/**
 * @brief Return a human-readable string for a power_state_t.
 */
const char *power_mgmt_state_to_str(power_state_t state);

/**
 * @brief Initialize power_mgmt and start the power_telemetry FreeRTOS task (prio 3).
 */
esp_err_t power_mgmt_init(void);

/**
 * @brief Get the latest thread-safe power telemetry snapshot.
 */
esp_err_t power_mgmt_get_status(power_status_t *out_status);

/**
 * @brief Reset the 10-minute inactivity timer (call on bus or CLI activity).
 */
void power_mgmt_notify_activity(void);

/**
 * @brief Configure ext1 wake-up (IO4 KLINE_RX / IO16 TWAI_RX LOW) and enter Deep Sleep.
 */
void __attribute__((noreturn)) power_mgmt_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MGMT_H */

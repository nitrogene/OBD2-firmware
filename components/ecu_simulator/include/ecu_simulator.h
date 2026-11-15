#ifndef ECU_SIMULATOR_H
#define ECU_SIMULATOR_H

#include "ecu_sim_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the global ECU simulator state instance.
 */
ecu_sim_state_t *ecu_simulator_state(void);

/**
 * @brief Initialize global ECU simulator state.
 */
esp_err_t ecu_simulator_init(void);

/**
 * @brief Start physical bus responder mode (MODE_ECU_SIMULATOR) on CAN and/or K-Line.
 */
esp_err_t ecu_simulator_start_bus(bool enable_can, bool enable_kline);

/**
 * @brief Stop physical bus responder tasks and release CAN/K-Line drivers.
 */
void ecu_simulator_stop_bus(void);

/**
 * @brief Query whether physical bus simulator mode is active.
 */
bool ecu_simulator_is_bus_running(void);

#ifdef __cplusplus
}
#endif

#endif /* ECU_SIMULATOR_H */

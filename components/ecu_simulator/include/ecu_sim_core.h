#ifndef ECU_SIM_CORE_H
#define ECU_SIM_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECU_SIM_MAX_DTCS 16U

typedef struct {
    uint16_t rpm;
    uint8_t speed_kmh;
    int16_t coolant_c;
    uint8_t load_pct;
    uint8_t throttle_pct;
    uint32_t vbat_mv;
    uint16_t o2_mv;
    char vin[18];
    uint8_t dtcs[ECU_SIM_MAX_DTCS][2];
    uint8_t dtc_count;
} ecu_sim_state_t;

void ecu_sim_init(ecu_sim_state_t *sim);
void ecu_sim_set_rpm(ecu_sim_state_t *sim, uint16_t rpm);
void ecu_sim_set_speed(ecu_sim_state_t *sim, uint8_t speed_kmh);
void ecu_sim_set_coolant(ecu_sim_state_t *sim, int16_t coolant_c);
void ecu_sim_set_o2_mv(ecu_sim_state_t *sim, uint16_t o2_mv);
int ecu_sim_inject_dtc(ecu_sim_state_t *sim, const char *dtc_str);
void ecu_sim_clear_dtcs(ecu_sim_state_t *sim);

/**
 * @brief Handle an OBD-II / UDS request in memory (returns 0 if response produced, -1 if no
 * data/blocked).
 */
int ecu_sim_handle_request(ecu_sim_state_t *sim, bool extended_unlocked, const uint8_t *req,
                           size_t req_len, uint8_t *out_resp, size_t max_resp,
                           size_t *out_resp_len);

#ifdef __cplusplus
}
#endif

#endif /* ECU_SIM_CORE_H */

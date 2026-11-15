#ifndef OBD2_AUTODETECT_H
#define OBD2_AUTODETECT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ELM327 ATSP protocol numbering.
 */
typedef enum {
    OBD2_PROTO_AUTO = 0,
    OBD2_PROTO_ISO9141_2 = 3,
    OBD2_PROTO_KWP_5BAUD = 4,
    OBD2_PROTO_KWP_FAST = 5,
    OBD2_PROTO_CAN_11B_500K = 6,
    OBD2_PROTO_CAN_29B_500K = 7,
    OBD2_PROTO_CAN_11B_250K = 8,
    OBD2_PROTO_CAN_29B_250K = 9,
} obd2_proto_id_t;

/**
 * @brief Return human-readable ELM327 ATDP protocol description string.
 */
const char *obd2_proto_name(obd2_proto_id_t proto);

/**
 * @brief Save last working protocol to NVS ("obd2" / "last_proto").
 */
esp_err_t obd2_nvs_save_proto(obd2_proto_id_t proto);

/**
 * @brief Load last working protocol from NVS (returns OBD2_PROTO_AUTO if unset).
 */
esp_err_t obd2_nvs_load_proto(obd2_proto_id_t *out_proto);

/**
 * @brief Execute ATSP0 auto-detection (NVS cached -> CAN listen-only/probe -> K-Line).
 */
esp_err_t obd2_autodetect_run(obd2_proto_id_t *out_proto);

#ifdef __cplusplus
}
#endif

#endif /* OBD2_AUTODETECT_H */

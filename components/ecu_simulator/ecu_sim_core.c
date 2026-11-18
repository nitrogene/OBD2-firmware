#include "ecu_sim_core.h"

#include <string.h>
#include "obd2_codec.h"

void ecu_sim_init(ecu_sim_state_t *sim) {
    if (sim == NULL) {
        return;
    }
    memset(sim, 0, sizeof(*sim));
    sim->rpm = 850U;
    sim->speed_kmh = 0U;
    sim->coolant_c = 88;
    sim->load_pct = 22U;
    sim->throttle_pct = 14U;
    sim->vbat_mv = 12600U;
    memcpy(sim->vin, "1G1JC5444R7252367", 18U);
}

void ecu_sim_set_rpm(ecu_sim_state_t *sim, uint16_t rpm) {
    if (sim != NULL) {
        sim->rpm = rpm;
    }
}

void ecu_sim_set_speed(ecu_sim_state_t *sim, uint8_t speed_kmh) {
    if (sim != NULL) {
        sim->speed_kmh = speed_kmh;
    }
}

void ecu_sim_set_coolant(ecu_sim_state_t *sim, int16_t coolant_c) {
    if (sim != NULL) {
        sim->coolant_c = coolant_c;
    }
}

int ecu_sim_inject_dtc(ecu_sim_state_t *sim, const char *dtc_str) {
    if (sim == NULL || dtc_str == NULL || sim->dtc_count >= ECU_SIM_MAX_DTCS) {
        return -1;
    }
    uint8_t raw[2] = {0};
    if (obd2_encode_dtc(dtc_str, raw) != 0) {
        return -1;
    }
    sim->dtcs[sim->dtc_count][0] = raw[0];
    sim->dtcs[sim->dtc_count][1] = raw[1];
    sim->dtc_count++;
    return 0;
}

void ecu_sim_clear_dtcs(ecu_sim_state_t *sim) {
    if (sim != NULL) {
        sim->dtc_count = 0U;
        memset(sim->dtcs, 0, sizeof(sim->dtcs));
    }
}

static int handle_mode01_or_02(const ecu_sim_state_t *sim, uint8_t mode, uint8_t pid,
                               uint8_t *out_resp, size_t max_resp, size_t *out_len) {
    if (max_resp < 6U) {
        return -1;
    }
    out_resp[0] = (uint8_t)(mode + 0x40U);
    out_resp[1] = pid;

    switch (pid) {
    case 0x00U:              /* Supported PIDs 01..20 */
        out_resp[2] = 0x18U; /* 04, 05 */
        out_resp[3] = 0x1AU; /* 0C, 0D, 0F */
        out_resp[4] = 0x80U; /* 11 */
        out_resp[5] = 0x01U; /* 20 */
        *out_len = 6U;
        return 0;
    case 0x04U:
        out_resp[2] = (uint8_t)((sim->load_pct * 255U) / 100U);
        *out_len = 3U;
        return 0;
    case 0x05U:
    case 0x0FU: {
        int16_t shifted = sim->coolant_c + 40;
        out_resp[2] = (shifted < 0) ? 0U : (shifted > 255 ? 255U : (uint8_t)shifted);
        *out_len = 3U;
        return 0;
    }
    case 0x0CU: {
        uint32_t raw_rpm = (uint32_t)sim->rpm * 4U;
        out_resp[2] = (uint8_t)((raw_rpm >> 8) & 0xFFU);
        out_resp[3] = (uint8_t)(raw_rpm & 0xFFU);
        *out_len = 4U;
        return 0;
    }
    case 0x0DU:
        out_resp[2] = sim->speed_kmh;
        *out_len = 3U;
        return 0;
    case 0x11U:
        out_resp[2] = (uint8_t)((sim->throttle_pct * 255U) / 100U);
        *out_len = 3U;
        return 0;
    case 0x20U: /* Supported PIDs 21..40 */
        out_resp[2] = 0x00U;
        out_resp[3] = 0x00U;
        out_resp[4] = 0x00U;
        out_resp[5] = 0x01U; /* 40 */
        *out_len = 6U;
        return 0;
    case 0x40U:              /* Supported PIDs 41..60 */
        out_resp[2] = 0x40U; /* 42 */
        out_resp[3] = 0x00U;
        out_resp[4] = 0x00U;
        out_resp[5] = 0x00U;
        *out_len = 6U;
        return 0;
    case 0x42U:
        out_resp[2] = (uint8_t)((sim->vbat_mv >> 8) & 0xFFU);
        out_resp[3] = (uint8_t)(sim->vbat_mv & 0xFFU);
        *out_len = 4U;
        return 0;
    default:
        return -1;
    }
}

int ecu_sim_handle_request(ecu_sim_state_t *sim, bool extended_unlocked, const uint8_t *req,
                           size_t req_len, uint8_t *out_resp, size_t max_resp,
                           size_t *out_resp_len) {
    if (sim == NULL || req == NULL || req_len == 0U || out_resp == NULL || out_resp_len == NULL) {
        return -1;
    }

    uint8_t sid = req[0];
    if (!obd2_is_service_allowed(sid, extended_unlocked)) {
        if (max_resp >= 3U) {
            out_resp[0] = 0x7FU;
            out_resp[1] = sid;
            out_resp[2] = 0x33U; /* NRC: SecurityAccessDenied */
            *out_resp_len = 3U;
            return 0;
        }
        return -1;
    }

    if ((sid == 0x01U || sid == 0x02U) && req_len >= 2U) {
        return handle_mode01_or_02(sim, sid, req[1], out_resp, max_resp, out_resp_len);
    }

    if (sid == 0x03U) {
        size_t needed = 2U + (size_t)sim->dtc_count * 2U;
        if (max_resp < needed) {
            return -1;
        }
        out_resp[0] = 0x43U;
        out_resp[1] = sim->dtc_count;
        for (uint8_t i = 0; i < sim->dtc_count; ++i) {
            out_resp[2U + i * 2U] = sim->dtcs[i][0];
            out_resp[3U + i * 2U] = sim->dtcs[i][1];
        }
        *out_resp_len = needed;
        return 0;
    }

    if (sid == 0x04U) {
        ecu_sim_clear_dtcs(sim);
        if (max_resp < 1U) {
            return -1;
        }
        out_resp[0] = 0x44U;
        *out_resp_len = 1U;
        return 0;
    }

    if (sid == 0x09U && req_len >= 2U && req[1] == 0x02U) {
        if (max_resp < 20U) {
            return -1;
        }
        out_resp[0] = 0x49U;
        out_resp[1] = 0x02U;
        out_resp[2] = 0x01U; /* 1 VIN item */
        memcpy(&out_resp[3], sim->vin, 17U);
        *out_resp_len = 20U;
        return 0;
    }

    if (extended_unlocked && sid == 0x10U && req_len >= 2U) {
        if (max_resp < 2U) {
            return -1;
        }
        out_resp[0] = 0x50U;
        out_resp[1] = req[1];
        *out_resp_len = 2U;
        return 0;
    }

    if (extended_unlocked && sid == 0x22U && req_len >= 3U) {
        uint16_t did = (uint16_t)(((uint16_t)req[1] << 8) | req[2]);
        if (did == 0xF190U) {
            if (max_resp < 20U) {
                return -1;
            }
            out_resp[0] = 0x62U;
            out_resp[1] = req[1];
            out_resp[2] = req[2];
            memcpy(&out_resp[3], sim->vin, 17U);
            *out_resp_len = 20U;
            return 0;
        }
        if (did == 0xF187U) {
            static const char part_no[] = "OBD2-S3-SIM";
            size_t len = sizeof(part_no) - 1U;
            if (max_resp < 3U + len) {
                return -1;
            }
            out_resp[0] = 0x62U;
            out_resp[1] = req[1];
            out_resp[2] = req[2];
            memcpy(&out_resp[3], part_no, len);
            *out_resp_len = 3U + len;
            return 0;
        }
        if (did == 0x010CU && max_resp >= 5U) {
            uint32_t raw_rpm = (uint32_t)sim->rpm * 4U;
            out_resp[0] = 0x62U;
            out_resp[1] = req[1];
            out_resp[2] = req[2];
            out_resp[3] = (uint8_t)((raw_rpm >> 8) & 0xFFU);
            out_resp[4] = (uint8_t)(raw_rpm & 0xFFU);
            *out_resp_len = 5U;
            return 0;
        }
        if (did == 0x010DU && max_resp >= 4U) {
            out_resp[0] = 0x62U;
            out_resp[1] = req[1];
            out_resp[2] = req[2];
            out_resp[3] = sim->speed_kmh;
            *out_resp_len = 4U;
            return 0;
        }
        if (max_resp >= 3U) {
            out_resp[0] = 0x7FU;
            out_resp[1] = sid;
            out_resp[2] = 0x31U; /* RequestOutOfRange */
            *out_resp_len = 3U;
            return 0;
        }
        return -1;
    }

    if (extended_unlocked && sid == 0x2EU && req_len >= 4U) {
        uint16_t did = (uint16_t)(((uint16_t)req[1] << 8) | req[2]);
        if (did == 0xF190U && req_len == 20U && max_resp >= 3U) {
            memcpy(sim->vin, &req[3], 17U);
            sim->vin[17] = '\0';
            out_resp[0] = 0x6EU;
            out_resp[1] = req[1];
            out_resp[2] = req[2];
            *out_resp_len = 3U;
            return 0;
        }
        if (max_resp >= 3U) {
            out_resp[0] = 0x7FU;
            out_resp[1] = sid;
            out_resp[2] = 0x31U;
            *out_resp_len = 3U;
            return 0;
        }
        return -1;
    }

    if (extended_unlocked && sid == 0x19U && req_len >= 2U) {
        uint8_t sub = req[1];
        if (sub == 0x01U && max_resp >= 6U) {
            out_resp[0] = 0x59U;
            out_resp[1] = 0x01U;
            out_resp[2] = 0xFFU; /* statusAvailabilityMask */
            out_resp[3] = 0x01U; /* ISO14229-1 DTCFormat */
            out_resp[4] = 0x00U;
            out_resp[5] = sim->dtc_count;
            *out_resp_len = 6U;
            return 0;
        }
        if (sub == 0x02U) {
            size_t needed = 3U + (size_t)sim->dtc_count * 4U;
            if (max_resp < needed) {
                return -1;
            }
            out_resp[0] = 0x59U;
            out_resp[1] = 0x02U;
            out_resp[2] = 0xFFU;
            for (uint8_t i = 0; i < sim->dtc_count; ++i) {
                size_t off = 3U + (size_t)i * 4U;
                out_resp[off] = sim->dtcs[i][0];
                out_resp[off + 1U] = sim->dtcs[i][1];
                out_resp[off + 2U] = 0x00U;
                out_resp[off + 3U] = 0x09U; /* confirmed + testFailed */
            }
            *out_resp_len = needed;
            return 0;
        }
    }

    if (extended_unlocked && sid == 0x14U) {
        ecu_sim_clear_dtcs(sim);
        if (max_resp < 1U) {
            return -1;
        }
        out_resp[0] = 0x54U;
        *out_resp_len = 1U;
        return 0;
    }

    /* KWP2000 TesterPresent (0x3E) */
    if (sid == 0x3EU) {
        if (max_resp < 1U) {
            return -1;
        }
        out_resp[0] = 0x7EU;
        *out_resp_len = 1U;
        return 0;
    }

    /* KWP2000 StartCommunication (0x81) / StopCommunication (0x82) */
    if (sid == 0x81U && max_resp >= 3U) {
        out_resp[0] = 0xC1U;
        out_resp[1] = 0xEFU;
        out_resp[2] = 0x8FU;
        *out_resp_len = 3U;
        return 0;
    }
    if (sid == 0x82U && max_resp >= 1U) {
        out_resp[0] = 0xC2U;
        *out_resp_len = 1U;
        return 0;
    }

    /* KWP2000 ReadDataByLocalIdentifier (0x21) */
    if (sid == 0x21U && req_len >= 2U && max_resp >= 6U) {
        uint32_t raw_rpm = (uint32_t)sim->rpm * 4U;
        int16_t shifted_clt = sim->coolant_c + 40;
        out_resp[0] = 0x61U;
        out_resp[1] = req[1];
        out_resp[2] = (uint8_t)((raw_rpm >> 8) & 0xFFU);
        out_resp[3] = (uint8_t)(raw_rpm & 0xFFU);
        out_resp[4] = sim->speed_kmh;
        out_resp[5] = (shifted_clt < 0) ? 0U : (shifted_clt > 255 ? 255U : (uint8_t)shifted_clt);
        *out_resp_len = 6U;
        return 0;
    }

    /* KWP2000 ReadDiagnosticTroubleCodesByStatus (0x18) */
    if (sid == 0x18U) {
        size_t needed = 2U + (size_t)sim->dtc_count * 3U;
        if (max_resp < needed) {
            return -1;
        }
        out_resp[0] = 0x58U;
        out_resp[1] = sim->dtc_count;
        for (uint8_t i = 0; i < sim->dtc_count; ++i) {
            size_t off = 2U + (size_t)i * 3U;
            out_resp[off] = sim->dtcs[i][0];
            out_resp[off + 1U] = sim->dtcs[i][1];
            out_resp[off + 2U] = 0x60U;
        }
        *out_resp_len = needed;
        return 0;
    }

    return -1;
}

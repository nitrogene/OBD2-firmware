#include "kwp2000.h"

#include <string.h>
#include "kline_proto.h"
#include "obd2_codec.h"

int kwp2000_build_service(uint8_t target_addr,
                          uint8_t sid,
                          const uint8_t *params,
                          uint8_t params_len,
                          bool extended_unlocked,
                          uint8_t *out_frame,
                          size_t max_len)
{
    if (out_frame == NULL || (params_len > 0U && params == NULL) || params_len > 62U) {
        return KWP_ERR_INVALID_ARG;
    }
    if (!obd2_is_service_allowed(sid, extended_unlocked)) {
        return KWP_ERR_SECURITY_BLOCKED;
    }

    uint8_t payload[64];
    payload[0] = sid;
    if (params_len > 0U) {
        memcpy(&payload[1], params, params_len);
    }

    int rc = kline_build_kwp_frame(0x80U, target_addr, KWP_TESTER_ADDR,
                                   payload, (uint8_t)(1U + params_len),
                                   out_frame, max_len);
    return (rc < 0) ? KWP_ERR_BUFFER_TOO_SMALL : rc;
}

int kwp2000_build_read_data(uint8_t target_addr,
                            uint8_t sid,
                            uint16_t id,
                            uint8_t *out_frame,
                            size_t max_len)
{
    if (sid == KWP_SID_READ_DATA_LOCAL) {
        uint8_t p[1] = {(uint8_t)(id & 0xFFU)};
        return kwp2000_build_service(target_addr, sid, p, 1U, false, out_frame, max_len);
    }
    if (sid == KWP_SID_READ_DATA_COMMON) {
        uint8_t p[2] = {(uint8_t)((id >> 8) & 0xFFU), (uint8_t)(id & 0xFFU)};
        return kwp2000_build_service(target_addr, sid, p, 2U, false, out_frame, max_len);
    }
    return KWP_ERR_INVALID_ARG;
}

int kwp2000_build_io_control(uint8_t target_addr,
                             uint8_t local_id,
                             uint8_t ctrl_param,
                             const uint8_t *opt_data,
                             uint8_t opt_len,
                             uint8_t *out_frame,
                             size_t max_len)
{
    if ((opt_len > 0U && opt_data == NULL) || opt_len > 60U) {
        return KWP_ERR_INVALID_ARG;
    }

    uint8_t params[62];
    params[0] = local_id;
    params[1] = ctrl_param;
    if (opt_len > 0U) {
        memcpy(&params[2], opt_data, opt_len);
    }
    return kwp2000_build_service(target_addr, KWP_SID_IO_CONTROL_LOCAL,
                                 params, (uint8_t)(2U + opt_len), false, out_frame, max_len);
}

int kwp2000_build_read_dtc(uint8_t target_addr,
                           uint8_t status_mask,
                           uint16_t group_of_dtc,
                           uint8_t *out_frame,
                           size_t max_len)
{
    uint8_t params[3] = {
        status_mask,
        (uint8_t)((group_of_dtc >> 8) & 0xFFU),
        (uint8_t)(group_of_dtc & 0xFFU),
    };
    return kwp2000_build_service(target_addr, KWP_SID_READ_DTC_STATUS,
                                 params, sizeof(params), false, out_frame, max_len);
}

int kwp2000_build_clear_dtc(uint8_t target_addr,
                            uint16_t group_of_dtc,
                            uint8_t *out_frame,
                            size_t max_len)
{
    uint8_t params[2] = {
        (uint8_t)((group_of_dtc >> 8) & 0xFFU),
        (uint8_t)(group_of_dtc & 0xFFU),
    };
    return kwp2000_build_service(target_addr, KWP_SID_CLEAR_DTC,
                                 params, sizeof(params), false, out_frame, max_len);
}

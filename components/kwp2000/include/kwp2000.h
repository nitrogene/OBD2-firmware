#ifndef KWP2000_H
#define KWP2000_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GM-Daewoo Kalos / Chevrolet Aveo K-Line module addresses */
#define KWP_TESTER_ADDR       0xF1U
#define KWP_MOD_ECM_PHYS      0x10U
#define KWP_MOD_ECM_5BAUD     0x11U
#define KWP_MOD_TCM           0x28U
#define KWP_MOD_ABS           0x58U
#define KWP_MOD_SDM_PRIMARY   0x50U
#define KWP_MOD_SDM_ALT       0x51U

/* KWP2000 OEM Service IDs */
#define KWP_SID_READ_DATA_LOCAL   0x21U
#define KWP_SID_READ_DATA_COMMON  0x22U
#define KWP_SID_IO_CONTROL_LOCAL  0x30U
#define KWP_SID_READ_DTC_STATUS   0x18U
#define KWP_SID_CLEAR_DTC         0x14U

#define KWP_ERR_INVALID_ARG       (-1)
#define KWP_ERR_SECURITY_BLOCKED  (-2)
#define KWP_ERR_BUFFER_TOO_SMALL  (-3)

/**
 * @brief Build a KWP2000 ReadDataByLocalIdentifier (0x21) or CommonId (0x22) frame.
 */
int kwp2000_build_read_data(uint8_t target_addr,
                            uint8_t sid,
                            uint16_t id,
                            uint8_t *out_frame,
                            size_t max_len);

/**
 * @brief Build a KWP2000 InputOutputControlByLocalIdentifier (0x30) frame.
 */
int kwp2000_build_io_control(uint8_t target_addr,
                             uint8_t local_id,
                             uint8_t ctrl_param,
                             const uint8_t *opt_data,
                             uint8_t opt_len,
                             uint8_t *out_frame,
                             size_t max_len);

/**
 * @brief Build a KWP2000 ReadDiagnosticTroubleCodesByStatus (0x18) frame.
 */
int kwp2000_build_read_dtc(uint8_t target_addr,
                           uint8_t status_mask,
                           uint16_t group_of_dtc,
                           uint8_t *out_frame,
                           size_t max_len);

/**
 * @brief Build a KWP2000 ClearDiagnosticInformation (0x14) frame.
 */
int kwp2000_build_clear_dtc(uint8_t target_addr,
                            uint16_t group_of_dtc,
                            uint8_t *out_frame,
                            size_t max_len);

/**
 * @brief Build an arbitrary KWP2000 service frame with security gating.
 *
 * Blocks sensitive services (0x27, 0x31, 0x34, 0x35, 0x36, 0x37) when
 * extended_unlocked is false, returning KWP_ERR_SECURITY_BLOCKED.
 */
int kwp2000_build_service(uint8_t target_addr,
                          uint8_t sid,
                          const uint8_t *params,
                          uint8_t params_len,
                          bool extended_unlocked,
                          uint8_t *out_frame,
                          size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* KWP2000_H */

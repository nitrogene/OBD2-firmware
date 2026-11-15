#ifndef OBD2_CODEC_H
#define OBD2_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SAE J1979 Modes */
#define OBD2_MODE_CURRENT_DATA        0x01U
#define OBD2_MODE_FREEZE_FRAME        0x02U
#define OBD2_MODE_READ_DTCS           0x03U
#define OBD2_MODE_CLEAR_DTCS          0x04U
#define OBD2_MODE_O2_MONITOR          0x05U
#define OBD2_MODE_ONBOARD_MON         0x06U
#define OBD2_MODE_PENDING_DTCS        0x07U
#define OBD2_MODE_CONTROL_OP          0x08U
#define OBD2_MODE_VEHICLE_INFO        0x09U
#define OBD2_MODE_PERMANENT_DTCS      0x0AU

/**
 * @brief Return true if a diagnostic SID is permitted under the current mode.
 *
 * Sensitive write/flash services (0x27 SecurityAccess, 0x31 RoutineControl,
 * 0x34 RequestDownload, 0x35 RequestUpload, 0x36 TransferData,
 * 0x37 RequestTransferExit) are blocked unless extended_unlocked is true.
 */
bool obd2_is_service_allowed(uint8_t sid, bool extended_unlocked);

/**
 * @brief Build a 2-byte OBD-II request (Mode + PID), or 1-byte (Mode 03/04/07/0A).
 *
 * Returns byte count written (1 or 2), or negative on error.
 */
int obd2_build_request(uint8_t mode, uint8_t pid, uint8_t *out_buf, size_t max_len);

/**
 * @brief Decode a 2-byte SAE J2012 DTC into a 5-char string (e.g. "P0100").
 */
int obd2_decode_dtc(uint8_t b0, uint8_t b1, char out_str[6]);

/**
 * @brief Encode a 5-char DTC string (e.g. "P0100") into 2 raw bytes.
 */
int obd2_encode_dtc(const char *dtc_str, uint8_t out_bytes[2]);

/**
 * @brief Scale common SAE J1979 Mode 01 PIDs into fixed-point value * 100.
 *
 * Supported PIDs: 0x04 (Load %), 0x05 (Coolant C), 0x0C (RPM),
 * 0x0D (Speed km/h), 0x0F (IAT C), 0x11 (Throttle %), 0x42 (Module mV * 100).
 * Returns 0 on success, -1 on unsupported PID or short data.
 */
int obd2_scale_mode01_pid(uint8_t pid,
                          const uint8_t *data,
                          uint8_t len,
                          int32_t *out_val_x100);

#ifdef __cplusplus
}
#endif

#endif /* OBD2_CODEC_H */

#include "obd2_codec.h"

#include <stddef.h>

bool obd2_is_service_allowed(uint8_t sid, bool extended_unlocked) {
    if (extended_unlocked) {
        return true;
    }

    switch (sid) {
    case 0x11U: /* ECUReset */
    case 0x27U: /* SecurityAccess */
    case 0x28U: /* CommunicationControl */
    case 0x2EU: /* WriteDataByIdentifier */
    case 0x2FU: /* InputOutputControlByIdentifier */
    case 0x30U: /* InputOutputControlByLocalIdentifier */
    case 0x31U: /* RoutineControl */
    case 0x34U: /* RequestDownload */
    case 0x35U: /* RequestUpload */
    case 0x36U: /* TransferData */
    case 0x37U: /* RequestTransferExit */
    case 0x3BU: /* WriteDataByLocalIdentifier */
    case 0x3DU: /* WriteMemoryByAddress */
    case 0x85U: /* ControlDTCSetting */
        return false;
    default:
        return true;
    }
}

int obd2_build_request(uint8_t mode, uint8_t pid, uint8_t *out_buf, size_t max_len) {
    if (out_buf == NULL || max_len < 1U || mode < 0x01U || mode > 0x0AU) {
        return -1;
    }

    if (mode == OBD2_MODE_READ_DTCS || mode == OBD2_MODE_CLEAR_DTCS ||
        mode == OBD2_MODE_PENDING_DTCS || mode == OBD2_MODE_PERMANENT_DTCS) {
        out_buf[0] = mode;
        return 1;
    }

    if (max_len < 2U) {
        return -1;
    }
    out_buf[0] = mode;
    out_buf[1] = pid;
    return 2;
}

static char hex_nibble(uint8_t n) {
    n &= 0x0FU;
    return (n < 10U) ? (char)('0' + n) : (char)('A' + (n - 10U));
}

static int parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

int obd2_decode_dtc(uint8_t b0, uint8_t b1, char out_str[6]) {
    static const char prefixes[4] = {'P', 'C', 'B', 'U'};
    if (out_str == NULL) {
        return -1;
    }

    out_str[0] = prefixes[(b0 >> 6) & 0x03U];
    out_str[1] = (char)('0' + ((b0 >> 4) & 0x03U));
    out_str[2] = hex_nibble(b0 & 0x0FU);
    out_str[3] = hex_nibble((b1 >> 4) & 0x0FU);
    out_str[4] = hex_nibble(b1 & 0x0FU);
    out_str[5] = '\0';
    return 0;
}

int obd2_encode_dtc(const char *dtc_str, uint8_t out_bytes[2]) {
    if (dtc_str == NULL || out_bytes == NULL) {
        return -1;
    }

    uint8_t sys = 0U;
    switch (dtc_str[0]) {
    case 'P':
    case 'p':
        sys = 0U;
        break;
    case 'C':
    case 'c':
        sys = 1U;
        break;
    case 'B':
    case 'b':
        sys = 2U;
        break;
    case 'U':
    case 'u':
        sys = 3U;
        break;
    default:
        return -1;
    }

    if (dtc_str[1] < '0' || dtc_str[1] > '3') {
        return -1;
    }
    int d2 = parse_hex_nibble(dtc_str[2]);
    int d3 = parse_hex_nibble(dtc_str[3]);
    int d4 = parse_hex_nibble(dtc_str[4]);
    if (d2 < 0 || d3 < 0 || d4 < 0 || dtc_str[5] != '\0') {
        return -1;
    }

    out_bytes[0] = (uint8_t)((sys << 6) | ((uint8_t)(dtc_str[1] - '0') << 4) | (uint8_t)d2);
    out_bytes[1] = (uint8_t)(((uint8_t)d3 << 4) | (uint8_t)d4);
    return 0;
}

int obd2_scale_mode01_pid(uint8_t pid, const uint8_t *data, uint8_t len, int32_t *out_val_x100) {
    if (data == NULL || out_val_x100 == NULL || len < 1U) {
        return -1;
    }

    switch (pid) {
    case 0x04U: /* Calculated engine load: A * 100 / 255 */
    case 0x11U: /* Throttle position: A * 100 / 255 */
        *out_val_x100 = ((int32_t)data[0] * 10000) / 255;
        return 0;

    case 0x05U: /* Engine coolant temp: A - 40 C */
    case 0x0FU: /* Intake air temp: A - 40 C */
        *out_val_x100 = ((int32_t)data[0] - 40) * 100;
        return 0;

    case 0x0CU: /* Engine RPM: (256*A + B) / 4 */
        if (len < 2U) {
            return -1;
        }
        *out_val_x100 = (((int32_t)data[0] * 256 + (int32_t)data[1]) * 100) / 4;
        return 0;

    case 0x0DU: /* Vehicle speed: A km/h */
        *out_val_x100 = (int32_t)data[0] * 100;
        return 0;

    case 0x42U: /* Control module voltage: (256*A + B) / 1000 V -> V*100 (rounded) */
        if (len < 2U) {
            return -1;
        }
        *out_val_x100 = ((int32_t)data[0] * 256 + (int32_t)data[1] + 5) / 10;
        return 0;

    default:
        return -1;
    }
}

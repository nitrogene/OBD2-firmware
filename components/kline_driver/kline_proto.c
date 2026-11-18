#include "kline_proto.h"

#include <string.h>

uint8_t kline_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0U;
    if (data == NULL) {
        return 0U;
    }
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return (uint8_t)(sum & 0xFFU);
}

int kline_build_kwp_frame(uint8_t fmt_base, uint8_t target, uint8_t source, const uint8_t *payload,
                          uint8_t payload_len, uint8_t *out_frame, size_t max_len) {
    if (payload == NULL || out_frame == NULL || payload_len == 0U || payload_len > 63U) {
        return -1;
    }

    size_t total = (size_t)payload_len + 4U; /* fmt + tgt + src + payload + cs */
    if (total > max_len) {
        return -2;
    }

    out_frame[0] = (uint8_t)((fmt_base & 0xC0U) | (payload_len & 0x3FU));
    out_frame[1] = target;
    out_frame[2] = source;
    memcpy(&out_frame[3], payload, payload_len);
    out_frame[3U + payload_len] = kline_checksum(out_frame, 3U + payload_len);
    return (int)total;
}

int kline_build_iso9141_frame(uint8_t hdr0, uint8_t target, uint8_t source, const uint8_t *payload,
                              uint8_t payload_len, uint8_t *out_frame, size_t max_len) {
    if (payload == NULL || out_frame == NULL || payload_len == 0U || payload_len > 7U) {
        return -1;
    }

    size_t total = (size_t)payload_len + 4U; /* hdr0 + tgt + src + payload + cs */
    if (total > max_len) {
        return -2;
    }

    out_frame[0] = hdr0 & 0x7FU;
    out_frame[1] = target;
    out_frame[2] = source;
    memcpy(&out_frame[3], payload, payload_len);
    out_frame[3U + payload_len] = kline_checksum(out_frame, 3U + payload_len);
    return (int)total;
}

int kline_parse_frame(const uint8_t *raw, size_t raw_len, kline_frame_t *out_frame) {
    if (raw == NULL || out_frame == NULL || raw_len < 5U) {
        return -1;
    }

    uint8_t expected_cs = kline_checksum(raw, raw_len - 1U);
    if (raw[raw_len - 1U] != expected_cs) {
        return -2;
    }

    memset(out_frame, 0, sizeof(*out_frame));
    out_frame->fmt = raw[0];
    out_frame->target = raw[1];
    out_frame->source = raw[2];
    out_frame->checksum = raw[raw_len - 1U];

    uint8_t len_field = raw[0] & 0x3FU;
    size_t hdr_len = 3U;
    size_t data_len = 0U;

    if ((raw[0] & 0x80U) != 0U) {
        /* KWP2000 header: either 3-byte (len in fmt) or 4-byte (additional length byte) */
        if (len_field == 0U) {
            if (raw_len < 6U) {
                return -3;
            }
            data_len = raw[3];
            hdr_len = 4U;
        } else {
            data_len = len_field;
        }
        if (hdr_len + data_len + 1U != raw_len) {
            return -4;
        }
    } else {
        /* ISO 9141-2 3-byte header (68 6A F1 ... cs) */
        data_len = raw_len - 4U;
    }

    if (data_len > sizeof(out_frame->payload)) {
        return -5;
    }
    memcpy(out_frame->payload, &raw[hdr_len], data_len);
    out_frame->payload_len = (uint8_t)data_len;
    return 0;
}

bool kline_verify_echo(const uint8_t *tx, const uint8_t *rx, size_t len) {
    if (tx == NULL || rx == NULL) {
        return false;
    }
    return memcmp(tx, rx, len) == 0;
}

kline_proto_type_t kline_classify_keybytes(uint8_t kb1, uint8_t kb2) {
    /* ISO 9141-2: 0x08 0x08 or 0x94 0x94 */
    if ((kb1 == 0x08U && kb2 == 0x08U) || (kb1 == 0x94U && kb2 == 0x94U)) {
        return KLINE_PROTO_ISO9141_2;
    }
    /* KWP2000 key bytes typically have KB2 == 0x8F (e.g., 0xE9 0x8F, 0xEA 0x8F, 0xEF 0x8F) */
    if (kb2 == 0x8FU || kb1 == 0x8FU) {
        return KLINE_PROTO_KWP2000;
    }
    return KLINE_PROTO_UNKNOWN;
}

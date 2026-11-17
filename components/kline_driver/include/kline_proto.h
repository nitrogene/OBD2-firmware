#ifndef KLINE_PROTO_H
#define KLINE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KLINE_MAX_FRAME_LEN 260U

typedef enum {
    KLINE_PROTO_UNKNOWN = 0,
    KLINE_PROTO_ISO9141_2,
    KLINE_PROTO_KWP2000,
} kline_proto_type_t;

typedef struct {
    uint8_t fmt;
    uint8_t target;
    uint8_t source;
    uint8_t payload[255];
    uint8_t payload_len;
    uint8_t checksum;
} kline_frame_t;

/**
 * @brief Compute 8-bit K-Line checksum (sum of all bytes mod 256).
 */
uint8_t kline_checksum(const uint8_t *data, size_t len);

/**
 * @brief Build a KWP2000 frame (header + payload + checksum).
 *
 * Uses format byte (0x80 | len) for physical addressing or (0xC0 | len) for functional.
 * Returns total frame length on success, or negative on error.
 */
int kline_build_kwp_frame(uint8_t fmt_base, uint8_t target, uint8_t source, const uint8_t *payload,
                          uint8_t payload_len, uint8_t *out_frame, size_t max_len);

/**
 * @brief Build an ISO 9141-2 3-byte header frame (hdr0 + target + source + payload + checksum).
 *
 * Typical tester request uses hdr0=0x68, target=0x6A, source=0xF1.
 * Typical ECU response uses hdr0=0x48, target=0x6B, source=0x10.
 * Returns total frame length on success, or negative on error.
 */
int kline_build_iso9141_frame(uint8_t hdr0, uint8_t target, uint8_t source, const uint8_t *payload,
                              uint8_t payload_len, uint8_t *out_frame, size_t max_len);

/**
 * @brief Parse and validate checksum of a raw KWP2000/ISO 9141-2 frame.
 *
 * Returns 0 on success, negative on invalid format or checksum mismatch.
 */
int kline_parse_frame(const uint8_t *raw, size_t raw_len, kline_frame_t *out_frame);

/**
 * @brief Verify local half-duplex TX-to-RX echo for collision detection.
 */
bool kline_verify_echo(const uint8_t *tx, const uint8_t *rx, size_t len);

/**
 * @brief Classify 5-baud init key bytes (KB1, KB2) into ISO 9141-2 or KWP2000.
 */
kline_proto_type_t kline_classify_keybytes(uint8_t kb1, uint8_t kb2);

#ifdef __cplusplus
}
#endif

#endif /* KLINE_PROTO_H */

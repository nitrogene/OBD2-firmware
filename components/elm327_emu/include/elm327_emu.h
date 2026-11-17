#ifndef ELM327_EMU_H
#define ELM327_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELM327_LINE_MAX 128U
#define ELM327_MAX_RESP_BYTES 512U

typedef enum {
    ELM_STATUS_OK = 0,
    ELM_STATUS_NO_DATA,
    ELM_STATUS_UNABLE_TO_CONNECT,
    ELM_STATUS_BUS_INIT_ERROR,
    ELM_STATUS_CAN_ERROR,
} elm327_bus_status_t;

typedef struct {
    void (*write_bytes)(const uint8_t *data, size_t len, void *ctx);
    uint32_t (*get_vbat_mv)(void *ctx);
    elm327_bus_status_t (*send_obd_req)(uint32_t tx_header, uint8_t protocol, bool extended_mode,
                                        const uint8_t *req, size_t req_len, uint8_t *out_resp,
                                        size_t max_resp, size_t *out_resp_len, uint32_t *out_rx_id,
                                        void *ctx);
    int (*set_protocol)(uint8_t protocol, void *ctx);
    void (*factory_reset)(void *ctx);
    void *user_ctx;
} elm327_callbacks_t;

typedef struct {
    elm327_callbacks_t cb;
    char line_buf[ELM327_LINE_MAX];
    size_t line_len;
    char last_cmd[ELM327_LINE_MAX];

    bool echo;
    bool linefeeds;
    bool spaces;
    bool headers;
    bool auto_format;
    bool extended_mode;
    bool sniff_enabled;

    uint8_t protocol;        /* ATSP 0..9 */
    uint8_t adaptive_timing; /* ATAT 0..2 */
    uint8_t timeout_st;      /* ATST (in 4 ms units, default 0x32 = 200 ms) */
    uint32_t tx_header;      /* ATSH default 0x7DF */
    uint32_t rx_filter_id;   /* ATCRA / ATCF */
    uint32_t rx_filter_mask; /* ATCM */
    uint32_t fc_header;      /* ATFCSH */
    uint8_t fc_data[5];      /* ATFCSD */
    uint8_t fc_data_len;
    uint8_t fc_mode; /* ATFCSM */

    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t err_frames;
} elm327_ctx_t;

/**
 * @brief Initialize ELM327 v1.5 emulator state to ATD defaults.
 */
void elm327_init(elm327_ctx_t *ctx, const elm327_callbacks_t *cb);

/**
 * @brief Reset AT settings to defaults (ATD / ATZ).
 */
void elm327_reset_defaults(elm327_ctx_t *ctx);

/**
 * @brief Feed incoming byte stream (e.g. from USB, BLE, or Wi-Fi TCP).
 */
void elm327_feed_bytes(elm327_ctx_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Format a CAN frame as an SLCAN ASCII line ('t<III><L><DATA>\r' or
 * 'T<IIIIIIII><L><DATA>\r').
 * @return Number of characters written (excluding NUL), or -1 on invalid args.
 */
int elm327_format_slcan_frame(uint32_t id, bool extd, const uint8_t *data, uint8_t dlc, char *out,
                              size_t max_out);

/**
 * @brief Callback type for AT+SIM:<KEY>=<VAL> commands (returns 0 on success, negative on error).
 */
typedef int (*elm327_sim_cmd_cb_t)(const char *kv_upper, void *user_ctx);

/**
 * @brief Configure an optional passkey required by AT+EXT=1,<PASSKEY> (NULL or "" disables passkey
 * check).
 */
void elm327_set_ext_passkey(const char *passkey);

/**
 * @brief Register a callback for wireless AT+SIM:<KEY>=<VAL> simulator control commands.
 */
void elm327_register_sim_cmd_cb(elm327_sim_cmd_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* ELM327_EMU_H */

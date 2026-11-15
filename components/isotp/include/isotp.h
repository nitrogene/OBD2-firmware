#ifndef ISOTP_H
#define ISOTP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ISOTP_MAX_PAYLOAD      4095U
#define ISOTP_PCI_SF           0x00U
#define ISOTP_PCI_FF           0x10U
#define ISOTP_PCI_CF           0x20U
#define ISOTP_PCI_FC           0x30U

#define ISOTP_FC_CTS           0x00U
#define ISOTP_FC_WAIT          0x01U
#define ISOTP_FC_OVFLW         0x02U

#define ISOTP_OK               0
#define ISOTP_ERR_INVALID_ARG  (-1)
#define ISOTP_ERR_BUSY         (-2)
#define ISOTP_ERR_OVERFLOW     (-3)
#define ISOTP_ERR_TIMEOUT      (-4)
#define ISOTP_ERR_SEQ          (-5)
#define ISOTP_ERR_NO_DATA      (-6)
#define ISOTP_ERR_TX_FAIL      (-7)

typedef int (*isotp_send_can_fn_t)(uint32_t can_id,
                                   bool extd,
                                   const uint8_t *data,
                                   uint8_t dlc,
                                   void *user_ctx);

typedef uint32_t (*isotp_get_ms_fn_t)(void *user_ctx);

typedef struct {
    uint32_t tx_id;
    uint32_t rx_id;
    bool extd;
    uint8_t block_size;      /**< FC BS advertised to peer (0 = unlimited) */
    uint8_t stmin_ms;        /**< FC STmin advertised to peer (0..127 ms) */
    bool pad_to_8;           /**< Pad CAN frames to 8 bytes */
    uint8_t padding_byte;    /**< Padding byte value (e.g. 0xAA or 0x55) */
    uint32_t n_bs_ms;        /**< Timeout waiting for FC (default 1000 ms) */
    uint32_t n_cr_ms;        /**< Timeout waiting for CF (default 1000 ms) */
    isotp_send_can_fn_t send_can;
    isotp_get_ms_fn_t get_ms;
    void *user_ctx;
} isotp_config_t;

typedef enum {
    ISOTP_TX_IDLE = 0,
    ISOTP_TX_WAIT_FC,
    ISOTP_TX_SENDING_CF,
    ISOTP_TX_ERROR,
} isotp_tx_state_t;

typedef enum {
    ISOTP_RX_IDLE = 0,
    ISOTP_RX_RECEIVING_CF,
    ISOTP_RX_COMPLETE,
    ISOTP_RX_ERROR,
} isotp_rx_state_t;

typedef struct {
    isotp_config_t cfg;

    /* TX state */
    isotp_tx_state_t tx_state;
    uint8_t tx_buf[ISOTP_MAX_PAYLOAD];
    uint16_t tx_len;
    uint16_t tx_offset;
    uint8_t tx_sn;
    uint8_t tx_bs_rem;
    uint8_t tx_peer_bs;
    uint32_t tx_peer_stmin_ms;
    uint32_t tx_last_ms;
    int tx_last_err;

    /* RX state */
    isotp_rx_state_t rx_state;
    uint8_t rx_buf[ISOTP_MAX_PAYLOAD];
    uint16_t rx_len;
    uint16_t rx_offset;
    uint8_t rx_sn;
    uint8_t rx_bs_rem;
    uint32_t rx_last_ms;
    int rx_last_err;
} isotp_link_t;

/**
 * @brief Initialize an ISO-TP link instance (zero dynamic allocation).
 */
int isotp_init(isotp_link_t *link, const isotp_config_t *cfg);

/**
 * @brief Start transmitting an ISO-TP payload (1..4095 bytes).
 */
int isotp_send(isotp_link_t *link, const uint8_t *payload, uint16_t len);

/**
 * @brief Feed a received CAN frame into the ISO-TP state machine.
 */
int isotp_on_can_rx(isotp_link_t *link,
                    uint32_t can_id,
                    bool extd,
                    const uint8_t *data,
                    uint8_t dlc);

/**
 * @brief Advance TX Consecutive Frames and check N_Bs / N_Cr timeouts.
 */
int isotp_poll(isotp_link_t *link);

/**
 * @brief Retrieve a completed RX message (returns ISOTP_ERR_NO_DATA if none).
 */
int isotp_receive(isotp_link_t *link,
                  uint8_t *out_buf,
                  uint16_t max_len,
                  uint16_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ISOTP_H */

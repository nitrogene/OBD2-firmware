#include "isotp.h"

#include <string.h>

static uint32_t get_now_ms(const isotp_link_t *link)
{
    return (link->cfg.get_ms != NULL) ? link->cfg.get_ms(link->cfg.user_ctx) : 0U;
}

static uint32_t decode_stmin_ms(uint8_t stmin_raw)
{
    if (stmin_raw <= 0x7FU) {
        return stmin_raw;
    }
    if (stmin_raw >= 0xF1U && stmin_raw <= 0xF9U) {
        return 1U;
    }
    return 127U;
}

static int send_can_frame(isotp_link_t *link, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[8];
    uint8_t dlc = len;

    if (link->cfg.pad_to_8) {
        memset(frame, link->cfg.padding_byte, sizeof(frame));
        memcpy(frame, payload, len);
        dlc = 8U;
    } else {
        memcpy(frame, payload, len);
    }

    int rc = link->cfg.send_can(link->cfg.tx_id, link->cfg.extd, frame, dlc, link->cfg.user_ctx);
    return (rc == 0) ? ISOTP_OK : ISOTP_ERR_TX_FAIL;
}

static int send_flow_control(isotp_link_t *link, uint8_t fs)
{
    uint8_t fc[3] = {
        (uint8_t)(ISOTP_PCI_FC | (fs & 0x0FU)),
        link->cfg.block_size,
        link->cfg.stmin_ms,
    };
    return send_can_frame(link, fc, sizeof(fc));
}

int isotp_init(isotp_link_t *link, const isotp_config_t *cfg)
{
    if (link == NULL || cfg == NULL || cfg->send_can == NULL) {
        return ISOTP_ERR_INVALID_ARG;
    }
    memset(link, 0, sizeof(*link));
    link->cfg = *cfg;
    if (link->cfg.n_bs_ms == 0U) {
        link->cfg.n_bs_ms = 1000U;
    }
    if (link->cfg.n_cr_ms == 0U) {
        link->cfg.n_cr_ms = 1000U;
    }
    return ISOTP_OK;
}

int isotp_send(isotp_link_t *link, const uint8_t *payload, uint16_t len)
{
    if (link == NULL || payload == NULL || len == 0U || len > ISOTP_MAX_PAYLOAD) {
        return ISOTP_ERR_INVALID_ARG;
    }
    if (link->tx_state == ISOTP_TX_WAIT_FC || link->tx_state == ISOTP_TX_SENDING_CF) {
        return ISOTP_ERR_BUSY;
    }

    if (len <= 7U) {
        uint8_t sf[8];
        sf[0] = (uint8_t)(ISOTP_PCI_SF | (len & 0x0FU));
        memcpy(&sf[1], payload, len);
        int rc = send_can_frame(link, sf, (uint8_t)(1U + len));
        link->tx_state = (rc == ISOTP_OK) ? ISOTP_TX_IDLE : ISOTP_TX_ERROR;
        link->tx_last_err = rc;
        return rc;
    }

    /* Multi-frame First Frame (FF) */
    memcpy(link->tx_buf, payload, len);
    link->tx_len = len;
    link->tx_offset = 6U;
    link->tx_sn = 1U;
    link->tx_last_err = ISOTP_OK;

    uint8_t ff[8];
    ff[0] = (uint8_t)(ISOTP_PCI_FF | ((len >> 8) & 0x0FU));
    ff[1] = (uint8_t)(len & 0xFFU);
    memcpy(&ff[2], payload, 6U);

    int rc = send_can_frame(link, ff, 8U);
    if (rc != ISOTP_OK) {
        link->tx_state = ISOTP_TX_ERROR;
        link->tx_last_err = rc;
        return rc;
    }

    link->tx_state = ISOTP_TX_WAIT_FC;
    link->tx_last_ms = get_now_ms(link);
    return ISOTP_OK;
}

static int handle_rx_sf(isotp_link_t *link, const uint8_t *data, uint8_t dlc)
{
    uint8_t len = data[0] & 0x0FU;
    if (len == 0U || len > 7U || (uint8_t)(1U + len) > dlc) {
        return ISOTP_ERR_INVALID_ARG;
    }
    memcpy(link->rx_buf, &data[1], len);
    link->rx_len = len;
    link->rx_offset = len;
    link->rx_state = ISOTP_RX_COMPLETE;
    link->rx_last_err = ISOTP_OK;
    return ISOTP_OK;
}

static int handle_rx_ff(isotp_link_t *link, const uint8_t *data, uint8_t dlc)
{
    if (dlc < 8U) {
        return ISOTP_ERR_INVALID_ARG;
    }
    uint16_t total_len = (uint16_t)(((uint16_t)(data[0] & 0x0FU) << 8) | data[1]);
    if (total_len < 8U || total_len > ISOTP_MAX_PAYLOAD) {
        (void)send_flow_control(link, ISOTP_FC_OVFLW);
        return ISOTP_ERR_OVERFLOW;
    }

    memcpy(link->rx_buf, &data[2], 6U);
    link->rx_len = total_len;
    link->rx_offset = 6U;
    link->rx_sn = 1U;
    link->rx_bs_rem = link->cfg.block_size;
    link->rx_state = ISOTP_RX_RECEIVING_CF;
    link->rx_last_ms = get_now_ms(link);
    link->rx_last_err = ISOTP_OK;

    return send_flow_control(link, ISOTP_FC_CTS);
}

static int handle_rx_cf(isotp_link_t *link, const uint8_t *data, uint8_t dlc)
{
    if (link->rx_state != ISOTP_RX_RECEIVING_CF || dlc < 2U) {
        return ISOTP_ERR_INVALID_ARG;
    }

    uint8_t sn = data[0] & 0x0FU;
    if (sn != link->rx_sn) {
        link->rx_state = ISOTP_RX_ERROR;
        link->rx_last_err = ISOTP_ERR_SEQ;
        return ISOTP_ERR_SEQ;
    }

    uint16_t rem = (uint16_t)(link->rx_len - link->rx_offset);
    uint8_t avail = (uint8_t)(dlc - 1U);
    uint8_t take = (rem < avail) ? (uint8_t)rem : avail;

    memcpy(&link->rx_buf[link->rx_offset], &data[1], take);
    link->rx_offset = (uint16_t)(link->rx_offset + take);
    link->rx_sn = (uint8_t)((link->rx_sn + 1U) & 0x0FU);
    link->rx_last_ms = get_now_ms(link);

    if (link->rx_offset >= link->rx_len) {
        link->rx_state = ISOTP_RX_COMPLETE;
        return ISOTP_OK;
    }

    if (link->cfg.block_size > 0U) {
        if (link->rx_bs_rem > 0U) {
            link->rx_bs_rem--;
        }
        if (link->rx_bs_rem == 0U) {
            link->rx_bs_rem = link->cfg.block_size;
            return send_flow_control(link, ISOTP_FC_CTS);
        }
    }
    return ISOTP_OK;
}

static int handle_rx_fc(isotp_link_t *link, const uint8_t *data, uint8_t dlc)
{
    if (link->tx_state != ISOTP_TX_WAIT_FC || dlc < 3U) {
        return ISOTP_ERR_INVALID_ARG;
    }

    uint8_t fs = data[0] & 0x0FU;
    if (fs == ISOTP_FC_CTS) {
        link->tx_peer_bs = data[1];
        link->tx_bs_rem = data[1];
        link->tx_peer_stmin_ms = decode_stmin_ms(data[2]);
        link->tx_state = ISOTP_TX_SENDING_CF;
        /* Allow immediate first CF when STmin == 0, or wait STmin */
        link->tx_last_ms = get_now_ms(link);
        return ISOTP_OK;
    }
    if (fs == ISOTP_FC_WAIT) {
        link->tx_last_ms = get_now_ms(link);
        return ISOTP_OK;
    }

    link->tx_state = ISOTP_TX_ERROR;
    link->tx_last_err = ISOTP_ERR_OVERFLOW;
    return ISOTP_ERR_OVERFLOW;
}

int isotp_on_can_rx(isotp_link_t *link,
                    uint32_t can_id,
                    bool extd,
                    const uint8_t *data,
                    uint8_t dlc)
{
    if (link == NULL || data == NULL || dlc == 0U || dlc > 8U) {
        return ISOTP_ERR_INVALID_ARG;
    }
    if (can_id != link->cfg.rx_id || extd != link->cfg.extd) {
        return ISOTP_OK;
    }

    uint8_t pci_type = data[0] & 0xF0U;
    switch (pci_type) {
    case ISOTP_PCI_SF:
        return handle_rx_sf(link, data, dlc);
    case ISOTP_PCI_FF:
        return handle_rx_ff(link, data, dlc);
    case ISOTP_PCI_CF:
        return handle_rx_cf(link, data, dlc);
    case ISOTP_PCI_FC:
        return handle_rx_fc(link, data, dlc);
    default:
        return ISOTP_ERR_INVALID_ARG;
    }
}

int isotp_poll(isotp_link_t *link)
{
    if (link == NULL) {
        return ISOTP_ERR_INVALID_ARG;
    }

    uint32_t now = get_now_ms(link);

    if (link->rx_state == ISOTP_RX_RECEIVING_CF) {
        if ((uint32_t)(now - link->rx_last_ms) >= link->cfg.n_cr_ms) {
            link->rx_state = ISOTP_RX_ERROR;
            link->rx_last_err = ISOTP_ERR_TIMEOUT;
            return ISOTP_ERR_TIMEOUT;
        }
    }

    if (link->tx_state == ISOTP_TX_WAIT_FC) {
        if ((uint32_t)(now - link->tx_last_ms) >= link->cfg.n_bs_ms) {
            link->tx_state = ISOTP_TX_ERROR;
            link->tx_last_err = ISOTP_ERR_TIMEOUT;
            return ISOTP_ERR_TIMEOUT;
        }
        return ISOTP_OK;
    }

    if (link->tx_state == ISOTP_TX_SENDING_CF) {
        if ((uint32_t)(now - link->tx_last_ms) < link->tx_peer_stmin_ms) {
            return ISOTP_OK;
        }

        uint16_t rem = (uint16_t)(link->tx_len - link->tx_offset);
        uint8_t chunk = (rem > 7U) ? 7U : (uint8_t)rem;

        uint8_t cf[8];
        cf[0] = (uint8_t)(ISOTP_PCI_CF | (link->tx_sn & 0x0FU));
        memcpy(&cf[1], &link->tx_buf[link->tx_offset], chunk);

        int rc = send_can_frame(link, cf, (uint8_t)(1U + chunk));
        if (rc != ISOTP_OK) {
            link->tx_state = ISOTP_TX_ERROR;
            link->tx_last_err = rc;
            return rc;
        }

        link->tx_offset = (uint16_t)(link->tx_offset + chunk);
        link->tx_sn = (uint8_t)((link->tx_sn + 1U) & 0x0FU);
        link->tx_last_ms = now;

        if (link->tx_offset >= link->tx_len) {
            link->tx_state = ISOTP_TX_IDLE;
            return ISOTP_OK;
        }

        if (link->tx_peer_bs > 0U) {
            if (link->tx_bs_rem > 0U) {
                link->tx_bs_rem--;
            }
            if (link->tx_bs_rem == 0U) {
                link->tx_state = ISOTP_TX_WAIT_FC;
            }
        }
    }

    return ISOTP_OK;
}

int isotp_receive(isotp_link_t *link,
                  uint8_t *out_buf,
                  uint16_t max_len,
                  uint16_t *out_len)
{
    if (link == NULL || out_buf == NULL || out_len == NULL) {
        return ISOTP_ERR_INVALID_ARG;
    }
    if (link->rx_state != ISOTP_RX_COMPLETE) {
        return ISOTP_ERR_NO_DATA;
    }
    if (link->rx_len > max_len) {
        return ISOTP_ERR_OVERFLOW;
    }

    memcpy(out_buf, link->rx_buf, link->rx_len);
    *out_len = link->rx_len;
    link->rx_state = ISOTP_RX_IDLE;
    link->rx_len = 0U;
    link->rx_offset = 0U;
    return ISOTP_OK;
}

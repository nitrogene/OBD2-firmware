#include "elm327_emu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit_raw(const elm327_ctx_t *ctx, const char *s, size_t len) {
    if (ctx->cb.write_bytes != NULL && len > 0U) {
        ctx->cb.write_bytes((const uint8_t *)s, len, ctx->cb.user_ctx);
    }
}

static void emit_str(const elm327_ctx_t *ctx, const char *s) {
    if (s != NULL) {
        emit_raw(ctx, s, strlen(s));
    }
}

static void emit_eol(const elm327_ctx_t *ctx) {
    emit_str(ctx, ctx->linefeeds ? "\r\n" : "\r");
}

static void emit_reply(const elm327_ctx_t *ctx, const char *text) {
    emit_str(ctx, text);
    emit_eol(ctx);
    emit_eol(ctx);
    emit_str(ctx, ">");
}

static const char *proto_description(uint8_t proto) {
    switch (proto) {
    case 0:
        return "AUTO";
    case 3:
        return "ISO 9141-2";
    case 4:
        return "ISO 14230-4 (KWP 5BAUD)";
    case 5:
        return "ISO 14230-4 (KWP FAST)";
    case 6:
        return "ISO 15765-4 (CAN 11/500)";
    case 7:
        return "ISO 15765-4 (CAN 29/500)";
    case 8:
        return "ISO 15765-4 (CAN 11/250)";
    case 9:
        return "ISO 15765-4 (CAN 29/250)";
    default:
        return "UNKNOWN";
    }
}

static int hex_val(char c) {
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

static int parse_hex_bytes(const char *s, uint8_t *out, size_t max_out) {
    size_t count = 0U;
    while (*s != '\0') {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        int hi = hex_val(s[0]);
        int lo = (s[1] != '\0') ? hex_val(s[1]) : -1;
        if (hi < 0 || lo < 0 || count >= max_out) {
            return -1;
        }
        out[count++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return (count > 0U) ? (int)count : -1;
}

static char s_ext_passkey[32] = {0};
static elm327_sim_cmd_cb_t s_sim_cmd_cb = NULL;

void elm327_set_ext_passkey(const char *passkey) {
    if (passkey == NULL) {
        s_ext_passkey[0] = '\0';
        return;
    }
    size_t i = 0U;
    for (; passkey[i] != '\0' && i + 1U < sizeof(s_ext_passkey); ++i) {
        s_ext_passkey[i] = (char)toupper((unsigned char)passkey[i]);
    }
    s_ext_passkey[i] = '\0';
}

void elm327_register_sim_cmd_cb(elm327_sim_cmd_cb_t cb) {
    s_sim_cmd_cb = cb;
}

static bool parse_can_id_hex(const char *s, size_t min_len, size_t max_len, uint32_t max_val,
                             uint32_t *out_val) {
    if (s == NULL || out_val == NULL) {
        return false;
    }
    size_t len = strlen(s);
    if (len < min_len || len > max_len) {
        return false;
    }
    uint32_t acc = 0U;
    for (size_t i = 0U; i < len; ++i) {
        int v = hex_val(s[i]);
        if (v < 0) {
            return false;
        }
        acc = (acc << 4) | (uint32_t)v;
    }
    if (acc > max_val) {
        return false;
    }
    *out_val = acc;
    return true;
}

static bool parse_sim_int(const char *val_str, long min_val, long max_val) {
    if (val_str == NULL || *val_str == '\0') {
        return false;
    }
    char *endptr = NULL;
    long v = strtol(val_str, &endptr, 10);
    if (endptr == val_str || *endptr != '\0') {
        return false;
    }
    return (v >= min_val && v <= max_val);
}

static bool validate_sim_kv(const char *kv) {
    if (strncmp(kv, "RPM=", 4) == 0) {
        return parse_sim_int(kv + 4, 0, 16383);
    }
    if (strncmp(kv, "SPD=", 4) == 0) {
        return parse_sim_int(kv + 4, 0, 255);
    }
    if (strncmp(kv, "SPEED=", 6) == 0) {
        return parse_sim_int(kv + 6, 0, 255);
    }
    if (strncmp(kv, "CLT=", 4) == 0) {
        return parse_sim_int(kv + 4, -40, 215);
    }
    if (strncmp(kv, "COOLANT=", 8) == 0) {
        return parse_sim_int(kv + 8, -40, 215);
    }
    if (strncmp(kv, "VBAT=", 5) == 0) {
        return parse_sim_int(kv + 5, 5000, 24000);
    }
    if (strncmp(kv, "DTC=", 4) == 0) {
        const char *dtc = kv + 4;
        return (strcmp(dtc, "CLEAR") == 0 || strlen(dtc) == 5U);
    }
    if (strcmp(kv, "START") == 0 || strcmp(kv, "STOP") == 0) {
        return true;
    }
    return false;
}

void elm327_reset_defaults(elm327_ctx_t *ctx) {
    ctx->echo = true;
    ctx->linefeeds = false;
    ctx->spaces = true;
    ctx->headers = false;
    ctx->auto_format = true;
    ctx->extended_mode = false;
    ctx->protocol = 0U;
    ctx->adaptive_timing = 1U;
    ctx->timeout_st = 0x32U;
    ctx->tx_header = 0x7DFU;
    ctx->rx_filter_id = 0U;
    ctx->rx_filter_mask = 0U;
    ctx->fc_header = 0x7E0U;
    ctx->fc_data_len = 0U;
    ctx->fc_mode = 0U;
}

void elm327_init(elm327_ctx_t *ctx, const elm327_callbacks_t *cb) {
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    if (cb != NULL) {
        ctx->cb = *cb;
    }
    elm327_reset_defaults(ctx);
}

static bool handle_at_command(elm327_ctx_t *ctx, const char *upper) {
    /* Extended custom mode toggle: AT+EXT=1[,<PASSKEY>] / AT+EXT=0 */
    if (strcmp(upper, "AT+EXT=1") == 0 || strncmp(upper, "AT+EXT=1,", 9) == 0) {
        if (s_ext_passkey[0] != '\0') {
            const char *provided = (upper[8] == ',') ? &upper[9] : "";
            if (strcmp(provided, s_ext_passkey) != 0) {
                emit_reply(ctx, "AUTH ERR");
                return true;
            }
        }
        ctx->extended_mode = true;
        emit_reply(ctx, "EXT OK");
        return true;
    }
    if (strcmp(upper, "AT+EXT=0") == 0) {
        ctx->extended_mode = false;
        ctx->sniff_enabled = false;
        emit_reply(ctx, "OK");
        return true;
    }
    if (ctx->extended_mode &&
        (strcmp(upper, "AT+SNIFF=1") == 0 || strcmp(upper, "AT+SNIFF=0") == 0)) {
        ctx->sniff_enabled = (upper[9] == '1');
        emit_reply(ctx, "OK");
        return true;
    }
    if (ctx->extended_mode && strcmp(upper, "AT+STATS") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "RX:%lu TX:%lu ERR:%lu", (unsigned long)ctx->rx_frames,
                 (unsigned long)ctx->tx_frames, (unsigned long)ctx->err_frames);
        emit_reply(ctx, buf);
        return true;
    }
    if (ctx->extended_mode &&
        (strcmp(upper, "AT+FACTORY") == 0 || strcmp(upper, "AT+FACTORY=CONFIRM") == 0)) {
        emit_reply(ctx, "FACTORY RESET");
        if (ctx->cb.factory_reset != NULL) {
            ctx->cb.factory_reset(ctx->cb.user_ctx);
        }
        return true;
    }
    if (strncmp(upper, "AT+SIM:", 7) == 0 || strncmp(upper, "AT+SIM=", 7) == 0) {
        const char *kv = &upper[7];
        if (!validate_sim_kv(kv)) {
            emit_reply(ctx, "?");
            return true;
        }
        if (s_sim_cmd_cb != NULL && s_sim_cmd_cb(kv, ctx->cb.user_ctx) != 0) {
            emit_reply(ctx, "?");
            return true;
        }
        emit_reply(ctx, "OK");
        return true;
    }

    if (strcmp(upper, "ATZ") == 0 || strcmp(upper, "ATWS") == 0) {
        elm327_reset_defaults(ctx);
        emit_reply(ctx, "ELM327 v1.5");
        return true;
    }
    if (strcmp(upper, "ATI") == 0) {
        emit_reply(ctx, "ELM327 v1.5");
        return true;
    }
    if (strcmp(upper, "AT@1") == 0) {
        emit_reply(ctx, "OBDII to RS232 Interpreter");
        return true;
    }
    if (strcmp(upper, "ATD") == 0) {
        elm327_reset_defaults(ctx);
        emit_reply(ctx, "OK");
        return true;
    }
    if (strcmp(upper, "ATPC") == 0) {
        emit_reply(ctx, "OK");
        return true;
    }

    /* Binary / simple toggles */
    if (strcmp(upper, "ATE0") == 0 || strcmp(upper, "ATE1") == 0) {
        ctx->echo = (upper[3] == '1');
        emit_reply(ctx, "OK");
        return true;
    }
    if (strcmp(upper, "ATL0") == 0 || strcmp(upper, "ATL1") == 0) {
        ctx->linefeeds = (upper[3] == '1');
        emit_reply(ctx, "OK");
        return true;
    }
    if (strcmp(upper, "ATS0") == 0 || strcmp(upper, "ATS1") == 0) {
        ctx->spaces = (upper[3] == '1');
        emit_reply(ctx, "OK");
        return true;
    }
    if (strcmp(upper, "ATH0") == 0 || strcmp(upper, "ATH1") == 0) {
        ctx->headers = (upper[3] == '1');
        emit_reply(ctx, "OK");
        return true;
    }
    if (strcmp(upper, "ATCAF0") == 0 || strcmp(upper, "ATCAF1") == 0) {
        ctx->auto_format = (upper[5] == '1');
        emit_reply(ctx, "OK");
        return true;
    }

    if (strcmp(upper, "ATRV") == 0) {
        uint32_t mv = (ctx->cb.get_vbat_mv != NULL) ? ctx->cb.get_vbat_mv(ctx->cb.user_ctx) : 0U;
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu.%luV", (unsigned long)(mv / 1000U),
                 (unsigned long)((mv % 1000U + 50U) / 100U));
        emit_reply(ctx, buf);
        return true;
    }

    if (strcmp(upper, "ATDP") == 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s%s", (ctx->protocol == 0U) ? "AUTO, " : "",
                 proto_description(ctx->protocol));
        emit_reply(ctx, buf);
        return true;
    }
    if (strcmp(upper, "ATDPN") == 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%s%X", (ctx->protocol == 0U) ? "A" : "",
                 (unsigned)ctx->protocol);
        emit_reply(ctx, buf);
        return true;
    }
    if (strncmp(upper, "ATSP", 4) == 0 && strlen(upper) == 5U) {
        int p = hex_val(upper[4]);
        if (p < 0 || p > 9) {
            emit_reply(ctx, "?");
            return true;
        }
        ctx->protocol = (uint8_t)p;
        if (ctx->cb.set_protocol != NULL &&
            ctx->cb.set_protocol(ctx->protocol, ctx->cb.user_ctx) != 0) {
            emit_reply(ctx, "UNABLE TO CONNECT");
            return true;
        }
        emit_reply(ctx, "OK");
        return true;
    }

    if (strncmp(upper, "ATAT", 4) == 0 && strlen(upper) == 5U && upper[4] >= '0' &&
        upper[4] <= '2') {
        ctx->adaptive_timing = (uint8_t)(upper[4] - '0');
        emit_reply(ctx, "OK");
        return true;
    }
    if (strncmp(upper, "ATST", 4) == 0 && strlen(upper) == 6U) {
        uint8_t val = 0;
        if (parse_hex_bytes(&upper[4], &val, 1U) == 1) {
            ctx->timeout_st = val;
            emit_reply(ctx, "OK");
            return true;
        }
        emit_reply(ctx, "?");
        return true;
    }

    if (strncmp(upper, "ATSH", 4) == 0) {
        uint32_t hdr = 0U;
        if (parse_can_id_hex(&upper[4], 3U, 8U, 0x1FFFFFFFU, &hdr)) {
            ctx->tx_header = hdr;
            emit_reply(ctx, "OK");
        } else {
            emit_reply(ctx, "?");
        }
        return true;
    }
    if (strcmp(upper, "ATCAR") == 0 || strcmp(upper, "ATCRA") == 0) {
        ctx->rx_filter_id = 0U;
        ctx->rx_filter_mask = 0U;
        emit_reply(ctx, "OK");
        return true;
    }
    if (strncmp(upper, "ATCRA", 5) == 0) {
        uint32_t fid = 0U;
        if (parse_can_id_hex(&upper[5], 3U, 8U, 0x1FFFFFFFU, &fid)) {
            ctx->rx_filter_id = fid;
            ctx->rx_filter_mask = (strlen(&upper[5]) > 3U) ? 0x1FFFFFFFU : 0x7FFU;
            emit_reply(ctx, "OK");
        } else {
            emit_reply(ctx, "?");
        }
        return true;
    }
    if (strncmp(upper, "ATCF", 4) == 0) {
        uint32_t fid = 0U;
        if (parse_can_id_hex(&upper[4], 3U, 8U, 0x1FFFFFFFU, &fid)) {
            ctx->rx_filter_id = fid;
            emit_reply(ctx, "OK");
        } else {
            emit_reply(ctx, "?");
        }
        return true;
    }
    if (strncmp(upper, "ATCM", 4) == 0) {
        uint32_t fmask = 0U;
        if (parse_can_id_hex(&upper[4], 3U, 8U, 0x1FFFFFFFU, &fmask)) {
            ctx->rx_filter_mask = fmask;
            emit_reply(ctx, "OK");
        } else {
            emit_reply(ctx, "?");
        }
        return true;
    }
    if (strncmp(upper, "ATFCSH", 6) == 0) {
        uint32_t fch = 0U;
        if (parse_can_id_hex(&upper[6], 3U, 8U, 0x1FFFFFFFU, &fch)) {
            ctx->fc_header = fch;
            emit_reply(ctx, "OK");
        } else {
            emit_reply(ctx, "?");
        }
        return true;
    }
    if (strncmp(upper, "ATFCSD", 6) == 0) {
        int n = parse_hex_bytes(&upper[6], ctx->fc_data, sizeof(ctx->fc_data));
        if (n > 0) {
            ctx->fc_data_len = (uint8_t)n;
            emit_reply(ctx, "OK");
            return true;
        }
        emit_reply(ctx, "?");
        return true;
    }
    if (strncmp(upper, "ATFCSM", 6) == 0 && strlen(upper) == 7U && upper[6] >= '0' &&
        upper[6] <= '2') {
        ctx->fc_mode = (uint8_t)(upper[6] - '0');
        emit_reply(ctx, "OK");
        return true;
    }

    if (strcmp(upper, "ATSI") == 0 || strcmp(upper, "ATFI") == 0) {
        if (ctx->cb.set_protocol != NULL) {
            uint8_t p = (strcmp(upper, "ATFI") == 0) ? 5U : 4U;
            if (ctx->cb.set_protocol(p, ctx->cb.user_ctx) == 0) {
                ctx->protocol = p;
                emit_reply(ctx, "BUS INIT: OK");
                return true;
            }
        }
        emit_reply(ctx, "BUS INIT: ERROR");
        return true;
    }

    if (strcmp(upper, "ATMA") == 0) {
        ctx->sniff_enabled = true;
        emit_reply(ctx, "STOPPED");
        return true;
    }
    if (strcmp(upper, "ATIGN") == 0) {
        emit_reply(ctx, "ON");
        return true;
    }

    emit_reply(ctx, "?");
    return true;
}

static void format_hex_bytes(const elm327_ctx_t *ctx, const uint8_t *bytes, size_t len, char *out,
                             size_t max_out) {
    size_t pos = 0U;
    for (size_t i = 0; i < len && pos + 4U < max_out; ++i) {
        int written = snprintf(&out[pos], max_out - pos,
                               (ctx->spaces && i + 1U < len) ? "%02X " : "%02X", bytes[i]);
        if (written > 0) {
            pos += (size_t)written;
        }
    }
    out[pos] = '\0';
}

static void emit_obd_response(const elm327_ctx_t *ctx, uint32_t rx_id, const uint8_t *resp,
                              size_t resp_len) {
    char line[96];
    if (resp_len <= 7U) {
        if (ctx->headers) {
            char hdr[16];
            snprintf(hdr, sizeof(hdr), (rx_id > 0x7FFU) ? "%08lX " : "%03lX ",
                     (unsigned long)rx_id);
            emit_str(ctx, hdr);
        }
        format_hex_bytes(ctx, resp, resp_len, line, sizeof(line));
        emit_reply(ctx, line);
        return;
    }

    /* Multi-line ISO-TP ELM327 format: total byte length, then 0:/1:/2:... */
    snprintf(line, sizeof(line), "%03X", (unsigned)resp_len);
    emit_str(ctx, line);
    emit_eol(ctx);

    size_t offset = 0U;
    uint8_t frame_idx = 0U;
    while (offset < resp_len) {
        size_t chunk = resp_len - offset;
        if (chunk > 7U) {
            chunk = 7U;
        }
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%X:", (unsigned)(frame_idx & 0x0FU));
        emit_str(ctx, prefix);
        format_hex_bytes(ctx, &resp[offset], chunk, line, sizeof(line));
        emit_str(ctx, line);
        offset += chunk;
        frame_idx++;
        if (offset < resp_len) {
            emit_eol(ctx);
        }
    }
    emit_eol(ctx);
    emit_eol(ctx);
    emit_str(ctx, ">");
}

static void process_command_line(elm327_ctx_t *ctx) {
    /* Trim trailing spaces; repeat last command if empty CR */
    while (ctx->line_len > 0U && ctx->line_buf[ctx->line_len - 1U] == ' ') {
        ctx->line_len--;
    }
    ctx->line_buf[ctx->line_len] = '\0';

    if (ctx->line_len == 0U) {
        if (ctx->last_cmd[0] == '\0') {
            emit_reply(ctx, "?");
            return;
        }
        strncpy(ctx->line_buf, ctx->last_cmd, sizeof(ctx->line_buf) - 1U);
        ctx->line_len = strlen(ctx->line_buf);
    } else {
        strncpy(ctx->last_cmd, ctx->line_buf, sizeof(ctx->last_cmd) - 1U);
    }

    /* Uppercase copy without spaces for AT command matching */
    char upper[ELM327_LINE_MAX];
    size_t u = 0U;
    for (size_t i = 0U; i < ctx->line_len && u + 1U < sizeof(upper); ++i) {
        char c = ctx->line_buf[i];
        if (c != ' ') {
            upper[u++] = (char)toupper((unsigned char)c);
        }
    }
    upper[u] = '\0';

    if (strncmp(upper, "AT", 2) == 0) {
        (void)handle_at_command(ctx, upper);
        return;
    }

    /* Hex OBD / UDS request */
    uint8_t req[64];
    int req_len = parse_hex_bytes(upper, req, sizeof(req));
    if (req_len <= 0) {
        emit_reply(ctx, "?");
        return;
    }

    if (ctx->cb.send_obd_req == NULL) {
        emit_reply(ctx, "NO DATA");
        return;
    }

    uint8_t resp[ELM327_MAX_RESP_BYTES];
    size_t resp_len = 0U;
    uint32_t rx_id = 0x7E8U;
    ctx->tx_frames++;
    elm327_bus_status_t st = ctx->cb.send_obd_req(ctx->tx_header, ctx->protocol, ctx->extended_mode,
                                                  req, (size_t)req_len, resp, sizeof(resp),
                                                  &resp_len, &rx_id, ctx->cb.user_ctx);

    switch (st) {
    case ELM_STATUS_OK:
        if (resp_len == 0U) {
            emit_reply(ctx, "NO DATA");
        } else {
            ctx->rx_frames++;
            emit_obd_response(ctx, rx_id, resp, resp_len);
        }
        break;
    case ELM_STATUS_UNABLE_TO_CONNECT:
        ctx->err_frames++;
        emit_reply(ctx, "UNABLE TO CONNECT");
        break;
    case ELM_STATUS_BUS_INIT_ERROR:
        ctx->err_frames++;
        emit_reply(ctx, "BUS INIT: ERROR");
        break;
    case ELM_STATUS_CAN_ERROR:
        ctx->err_frames++;
        emit_reply(ctx, "CAN ERROR");
        break;
    case ELM_STATUS_NO_DATA:
    default:
        emit_reply(ctx, "NO DATA");
        break;
    }
}

void elm327_feed_bytes(elm327_ctx_t *ctx, const uint8_t *data, size_t len) {
    if (ctx == NULL || data == NULL) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = data[i];
        if (ch == '\n') {
            continue;
        }
        if (ctx->echo) {
            emit_raw(ctx, (const char *)&ch, 1U);
        }
        if (ch == '\r') {
            if (ctx->echo && ctx->linefeeds) {
                emit_raw(ctx, "\n", 1U);
            }
            process_command_line(ctx);
            ctx->line_len = 0U;
            continue;
        }
        if (ctx->line_len + 1U < ELM327_LINE_MAX) {
            ctx->line_buf[ctx->line_len++] = (char)ch;
        } else {
            ctx->line_len = 0U;
            emit_reply(ctx, "BUFFER FULL");
        }
    }
}

int elm327_format_slcan_frame(uint32_t id, bool extd, const uint8_t *data, uint8_t dlc, char *out,
                              size_t max_out) {
    if (out == NULL || dlc > 8U || (dlc > 0U && data == NULL)) {
        return -1;
    }
    size_t needed = (extd ? 10U : 5U) + (size_t)dlc * 2U + 2U;
    if (max_out < needed) {
        return -1;
    }
    int pos =
        extd ? snprintf(out, max_out, "T%08lX%u", (unsigned long)(id & 0x1FFFFFFFUL), (unsigned)dlc)
             : snprintf(out, max_out, "t%03lX%u", (unsigned long)(id & 0x7FFUL), (unsigned)dlc);
    if (pos < 0) {
        return -1;
    }
    for (uint8_t i = 0; i < dlc; ++i) {
        int n = snprintf(&out[pos], max_out - (size_t)pos, "%02X", data[i]);
        if (n < 0) {
            return -1;
        }
        pos += n;
    }
    out[pos++] = '\r';
    out[pos] = '\0';
    return pos;
}

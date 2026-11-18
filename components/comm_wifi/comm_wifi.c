#include "comm_wifi.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm_ble.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_mgmt.h"
#include "transport_stream.h"

static const char *TAG = "comm_wifi";

#define NVS_NAMESPACE "comm_wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"

static SemaphoreHandle_t s_wifi_mutex = NULL;
static int s_client_sock = -1;
static char s_ssid[33] = COMM_WIFI_DEFAULT_SSID;
static char s_pass[65] = COMM_WIFI_DEFAULT_PASS;
static char s_sta_ssid[33] = "";
static char s_sta_pass[65] = "";

static void wifi_lock(void) {
    if (s_wifi_mutex != NULL) {
        (void)xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void) {
    if (s_wifi_mutex != NULL) {
        (void)xSemaphoreGive(s_wifi_mutex);
    }
}

static bool const_time_str_eq(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a == 0U || len_a != len_b) {
        return false;
    }
    uint8_t diff = 0U;
    for (size_t i = 0U; i < len_a; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0U;
}

static void html_escape(const char *src, char *dst, size_t max_dst) {
    if (dst == NULL || max_dst == 0U) {
        return;
    }
    size_t out = 0U;
    for (size_t i = 0U; src != NULL && src[i] != '\0' && out + 6U < max_dst; ++i) {
        char c = src[i];
        if (c == '&') {
            memcpy(&dst[out], "&amp;", 5U);
            out += 5U;
        } else if (c == '<') {
            memcpy(&dst[out], "&lt;", 4U);
            out += 4U;
        } else if (c == '>') {
            memcpy(&dst[out], "&gt;", 4U);
            out += 4U;
        } else if (c == '\'') {
            memcpy(&dst[out], "&#39;", 5U);
            out += 5U;
        } else if (c == '"') {
            memcpy(&dst[out], "&quot;", 6U);
            out += 6U;
        } else {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
}

static void comm_wifi_load_credentials(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    size_t ssid_len = sizeof(s_ssid);
    if (nvs_get_str(handle, NVS_KEY_SSID, s_ssid, &ssid_len) != ESP_OK || s_ssid[0] == '\0') {
        strncpy(s_ssid, COMM_WIFI_DEFAULT_SSID, sizeof(s_ssid) - 1U);
        s_ssid[sizeof(s_ssid) - 1U] = '\0';
    }

    size_t pass_len = sizeof(s_pass);
    if (nvs_get_str(handle, NVS_KEY_PASS, s_pass, &pass_len) != ESP_OK || strlen(s_pass) < 8U) {
        strncpy(s_pass, COMM_WIFI_DEFAULT_PASS, sizeof(s_pass) - 1U);
        s_pass[sizeof(s_pass) - 1U] = '\0';
    }

    size_t sta_ssid_len = sizeof(s_sta_ssid);
    if (nvs_get_str(handle, NVS_KEY_STA_SSID, s_sta_ssid, &sta_ssid_len) != ESP_OK) {
        s_sta_ssid[0] = '\0';
    }

    size_t sta_pass_len = sizeof(s_sta_pass);
    if (nvs_get_str(handle, NVS_KEY_STA_PASS, s_sta_pass, &sta_pass_len) != ESP_OK) {
        s_sta_pass[0] = '\0';
    }

    nvs_close(handle);
}

static esp_err_t comm_wifi_transport_tx(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_lock();
    int sock = s_client_sock;
    if (sock < 0) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    size_t sent = 0U;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            wifi_unlock();
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }
    wifi_unlock();
    return ESP_OK;
}

static void url_decode_inplace(char *str) {
    char *src = str;
    char *dst = str;
    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtoul(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool extract_form_field(const char *body, const char *key, char *out, size_t max_out) {
    size_t key_len = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1U;
            const char *amp = strchr(val, '&');
            size_t len = (amp != NULL) ? (size_t)(amp - val) : strlen(val);
            if (len >= max_out) {
                len = max_out - 1U;
            }
            memcpy(out, val, len);
            out[len] = '\0';
            url_decode_inplace(out);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    return false;
}

static bool http_check_auth(httpd_req_t *req, const char *form_body) {
    char expected[65] = {0};
    wifi_lock();
    strncpy(expected, s_pass, sizeof(expected) - 1U);
    wifi_unlock();

    char hdr_token[65] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Auth", hdr_token, sizeof(hdr_token)) == ESP_OK) {
        if (const_time_str_eq(hdr_token, expected)) {
            return true;
        }
    }

    if (form_body != NULL) {
        char form_pass[65] = {0};
        if (extract_form_field(form_body, "admin_pass", form_pass, sizeof(form_pass)) &&
            const_time_str_eq(form_pass, expected)) {
            return true;
        }
    }

    return false;
}

static esp_err_t http_get_root_handler(httpd_req_t *req) {
    char esc_ssid[196];
    char esc_sta[196];
    wifi_lock();
    html_escape(s_ssid, esc_ssid, sizeof(esc_ssid));
    html_escape((s_sta_ssid[0] != '\0') ? s_sta_ssid : "disabled", esc_sta, sizeof(esc_sta));
    wifi_unlock();

    size_t html_cap = 3072U;
    char *html = (char *)malloc(html_cap);
    if (html == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    snprintf(html, html_cap,
             "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<title>OBD2-Scanner Config</title>"
             "<style>body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 12px}"
             "input,button{width:100%%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
             "fieldset{margin-bottom:16px;padding:12px}</style></head><body>"
             "<h2>OBD2-Scanner (ELM327)</h2>"
             "<p>SoftAP: <b>%s</b> (" COMM_WIFI_AP_IP ":35000) | STA: <b>%s</b></p>"
             "<form method='POST' action='/config'><fieldset><legend>Wi-Fi &amp; BLE</legend>"
             "Current AP Password (Auth):<input id='admin_pass' name='admin_pass' type='password' "
             "required maxlength='63'>"
             "AP SSID:<input name='ap_ssid' value='%s' maxlength='32'>"
             "New AP Password:<input name='ap_pass' type='password' value='' "
             "placeholder='Unchanged if empty (min 8 chars)' maxlength='63'>"
             "STA SSID (optional):<input name='sta_ssid' value='%s' maxlength='32'>"
             "STA Password:<input name='sta_pass' type='password' value='' placeholder='Unchanged "
             "if empty' maxlength='63'>"
             "BLE Name:<input name='ble_name' maxlength='31' placeholder='OBD2-ELM327'>"
             "<button type='submit'>Save Config</button></fieldset></form>"
             "<form method='POST' action='/ota' enctype='application/octet-stream'>"
             "<fieldset><legend>OTA Firmware Update (.bin)</legend>"
             "<input type='file' id='fw' accept='.bin'>"
             "<button type='button' onclick='uploadOta()'>Upload &amp; Flash</button>"
             "<div id='ota_st'></div></fieldset></form>"
             "<form method='POST' action='/factory-reset'>"
             "<input name='admin_pass' type='password' required placeholder='Current AP Password "
             "to confirm reset' maxlength='63'>"
             "<button type='submit'>Factory Reset NVS &amp; Reboot</button></form>"
             "<script>async function uploadOta(){"
             "const f=document.getElementById('fw').files[0];if(!f)return;"
             "const p=document.getElementById('admin_pass').value;"
             "document.getElementById('ota_st').innerText='Uploading...';"
             "const r=await fetch('/ota',{method:'POST',headers:{'X-OTA-Auth':p},body:f});"
             "document.getElementById('ota_st').innerText=await r.text();}</script>"
             "</body></html>",
             esc_ssid, esc_sta, esc_ssid, esc_sta);

    httpd_resp_set_type(req, "text/html");
    esp_err_t res = httpd_resp_sendstr(req, html);
    free(html);
    return res;
}

static esp_err_t http_post_config_handler(httpd_req_t *req) {
    char body[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, (size_t)total);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        return ESP_FAIL;
    }
    body[received] = '\0';

    if (!http_check_auth(req, body)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    char ap_ssid[33] = {0}, ap_pass[65] = {0};
    char sta_ssid[33] = {0}, sta_pass[65] = {0};
    char ble_name[32] = {0};

    if (extract_form_field(body, "ap_ssid", ap_ssid, sizeof(ap_ssid)) && ap_ssid[0] != '\0') {
        (void)extract_form_field(body, "ap_pass", ap_pass, sizeof(ap_pass));
        const char *pass_to_save = (ap_pass[0] != '\0') ? ap_pass : s_pass;
        if (comm_wifi_set_ap_credentials(ap_ssid, pass_to_save) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Invalid AP SSID or password (min 8 chars)");
            return ESP_FAIL;
        }
    }
    if (extract_form_field(body, "sta_ssid", sta_ssid, sizeof(sta_ssid))) {
        (void)extract_form_field(body, "sta_pass", sta_pass, sizeof(sta_pass));
        const char *sta_pass_to_save = (sta_pass[0] != '\0') ? sta_pass : s_sta_pass;
        (void)comm_wifi_set_sta_credentials(sta_ssid, sta_pass_to_save);
    }
    if (extract_form_field(body, "ble_name", ble_name, sizeof(ble_name)) && ble_name[0] != '\0') {
        (void)comm_ble_set_name(ble_name);
    }

    httpd_resp_sendstr(req, "Saved to NVS. Reboot to apply Wi-Fi changes.");
    return ESP_OK;
}

static esp_err_t http_post_ota_handler(httpd_req_t *req) {
    if (!http_check_auth(req, NULL)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized OTA request");
        return ESP_FAIL;
    }

    /* Enforce vehicle battery voltage gate before erasing/writing flash (reject if < 11.8 V or
     * > 15.5 V) */
    power_status_t pwr = {0};
    if (power_mgmt_get_status(&pwr) == ESP_OK) {
        if (pwr.state == POWER_STATE_UNDERVOLTAGE_CRITICAL ||
            pwr.state == POWER_STATE_OVERVOLTAGE) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "OTA blocked: unsafe vehicle battery voltage (< 11.8V or > 15.5V)");
            return ESP_FAIL;
        }
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    if (req->content_len <= (int)sizeof(esp_image_header_t) ||
        (uint32_t)req->content_len > update_part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid OTA image size");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    bool first_chunk = true;
    while (remaining > 0) {
        int to_read = (remaining > (int)sizeof(buf)) ? (int)sizeof(buf) : remaining;
        int n = httpd_req_recv(req, buf, (size_t)to_read);
        if (n <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA recv failed");
            return ESP_FAIL;
        }
        if (first_chunk) {
            if ((uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ESP-IDF image magic byte");
                return ESP_FAIL;
            }
            first_chunk = false;
        }
        power_mgmt_notify_activity();
        err = esp_ota_write(ota_handle, buf, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }
        remaining -= n;
    }

    err = esp_ota_end(ota_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_part);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA finalize failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OTA OK, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_post_factory_handler(httpd_req_t *req) {
    char body[128] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) {
        int r = httpd_req_recv(req, body, (size_t)total);
        if (r > 0) {
            body[r] = '\0';
        }
    }
    if (!http_check_auth(req, body)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized factory reset");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Factory reset OK, rebooting...");
    (void)nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static void comm_wifi_start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start HTTP config/OTA server");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = http_get_root_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_cfg = {.uri = "/config",
                                        .method = HTTP_POST,
                                        .handler = http_post_config_handler,
                                        .user_ctx = NULL};
    static const httpd_uri_t uri_ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = http_post_ota_handler, .user_ctx = NULL};
    static const httpd_uri_t uri_rst = {.uri = "/factory-reset",
                                        .method = HTTP_POST,
                                        .handler = http_post_factory_handler,
                                        .user_ctx = NULL};

    (void)httpd_register_uri_handler(server, &uri_root);
    (void)httpd_register_uri_handler(server, &uri_cfg);
    (void)httpd_register_uri_handler(server, &uri_ota);
    (void)httpd_register_uri_handler(server, &uri_rst);
    ESP_LOGI(TAG, "Web config & OTA server listening on port 80");
}

static void comm_wifi_tcp_task(void *arg) {
    (void)arg;

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* Bind exclusively to SoftAP IP (COMM_WIFI_AP_IP) so TCP :35000 is never exposed on STA
     * networks */
    addr.sin_addr.s_addr = inet_addr(COMM_WIFI_AP_IP);
    addr.sin_port = htons(COMM_WIFI_TCP_PORT);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ELM327 TCP server listening on %s:%d", COMM_WIFI_AP_IP, COMM_WIFI_TCP_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client < 0) {
            ESP_LOGW(TAG, "accept() failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        wifi_lock();
        s_client_sock = client;
        wifi_unlock();
        ESP_LOGI(TAG, "TCP client connected (%s)", inet_ntoa(client_addr.sin_addr));

        uint8_t rx_buf[256];
        for (;;) {
            int n = recv(client, rx_buf, sizeof(rx_buf), 0);
            if (n > 0) {
                esp_err_t err = transport_stream_feed_rx(TRANSPORT_ID_WIFI, rx_buf, (size_t)n);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Wi-Fi RX dropped (%s)", esp_err_to_name(err));
                }
            } else {
                break;
            }
        }

        ESP_LOGI(TAG, "TCP client disconnected");
        wifi_lock();
        s_client_sock = -1;
        close(client);
        wifi_unlock();
        transport_stream_release(TRANSPORT_ID_WIFI);
    }
}

esp_err_t comm_wifi_init(void) {
    if (s_wifi_mutex == NULL) {
        s_wifi_mutex = xSemaphoreCreateMutex();
        if (s_wifi_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Confirm running OTA image is valid so bootloader does not roll back on next reset */
    (void)esp_ota_mark_app_valid_cancel_rollback();

    wifi_lock();
    comm_wifi_load_credentials();
    wifi_unlock();

    esp_err_t err = transport_stream_register_tx(TRANSPORT_ID_WIFI, comm_wifi_transport_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = inet_addr(COMM_WIFI_AP_IP);
    ip_info.gw.addr = inet_addr(COMM_WIFI_AP_IP);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    err = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    bool has_sta = (s_sta_ssid[0] != '\0');
    if (has_sta) {
        (void)esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    strncpy((char *)ap_cfg.ap.ssid, s_ssid, sizeof(ap_cfg.ap.ssid) - 1U);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 2;
    if (strlen(s_pass) < 8U) {
        strncpy(s_pass, COMM_WIFI_DEFAULT_PASS, sizeof(s_pass) - 1U);
        s_pass[sizeof(s_pass) - 1U] = '\0';
    }
    strncpy((char *)ap_cfg.ap.password, s_pass, sizeof(ap_cfg.ap.password) - 1U);
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(has_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (has_sta) {
        wifi_config_t sta_cfg;
        memset(&sta_cfg, 0, sizeof(sta_cfg));
        strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1U);
        strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password) - 1U);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (has_sta) {
        (void)esp_wifi_connect();
    }

    BaseType_t ok = xTaskCreate(comm_wifi_tcp_task, "comm_wifi_tcp", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    comm_wifi_start_http_server();
    ESP_LOGI(TAG, "SoftAP '%s' started at %s:%d (WPA2-PSK)", s_ssid, COMM_WIFI_AP_IP,
             COMM_WIFI_TCP_PORT);
    return ESP_OK;
}

esp_err_t comm_wifi_set_ap_credentials(const char *ssid, const char *pass) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32U || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pass_len = strlen(pass);
    /* Disallow empty/open SoftAP passwords: require 8..63 ASCII characters for WPA2-PSK */
    if (pass_len < 8U || pass_len > 63U) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        wifi_lock();
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1U);
        s_ssid[sizeof(s_ssid) - 1U] = '\0';
        strncpy(s_pass, pass, sizeof(s_pass) - 1U);
        s_pass[sizeof(s_pass) - 1U] = '\0';
        wifi_unlock();
    }
    return err;
}

esp_err_t comm_wifi_set_sta_credentials(const char *ssid, const char *pass) {
    if (ssid == NULL || strlen(ssid) > 32U || pass == NULL || strlen(pass) > 63U) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, NVS_KEY_STA_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_STA_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        wifi_lock();
        strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1U);
        s_sta_ssid[sizeof(s_sta_ssid) - 1U] = '\0';
        strncpy(s_sta_pass, pass, sizeof(s_sta_pass) - 1U);
        s_sta_pass[sizeof(s_sta_pass) - 1U] = '\0';
        wifi_unlock();
    }
    return err;
}

bool comm_wifi_is_client_connected(void) {
    wifi_lock();
    bool connected = (s_client_sock >= 0);
    wifi_unlock();
    return connected;
}

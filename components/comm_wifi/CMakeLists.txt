idf_component_register(
    SRCS "comm_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update comm_ble esp_http_server esp_wifi esp_netif esp_event lwip nvs_flash transport_stream
)
